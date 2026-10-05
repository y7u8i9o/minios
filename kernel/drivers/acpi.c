/* ACPI with the AML interpreter of uACPI (docs/design/acpi.md, V1 of
 * docs/plan/release-0.6.0.md): the namespace, the sleep state S5, the reset
 * register and the power button.
 *
 * The power button reaches the kernel in one of three ways. A PC reports
 * it as the fixed power button event of the PM1 registers. The hardware
 * reduced ACPI of QEMU virt on aarch64 has a Generic Event Device
 * (ACPI0013), whose interrupt runs the method _EVT, and _EVT notifies the
 * power button device (PNP0C0C) with the value 0x80. aarch64 with a
 * device tree has a GPIO key (arch/aarch64/powerkey.c). Each way ends in
 * acpi_power_button, which sends init the signal of shutdown. */
#define KLOG_SUBSYS "acpi"
#include <drivers/acpi.h>
#include <drivers/devinfo.h>
#include <arch/machine.h>
#include <arch/cpu.h>
#include <arch/irq.h>
#include <boot.h>
#include <klog.h>
#include <ipc/signal.h>
#include <sched/proc.h>
#include <minios/abi.h>
#include <uacpi/uacpi.h>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>
#include <uacpi/event.h>
#include <uacpi/notify.h>
#include <uacpi/sleep.h>
#include <uacpi/utilities.h>
#include <uacpi/resources.h>

/* Written once by acpi_init before the events are enabled. */
static bool loaded;
static bool reset_register;     /* the FADT has a reset register */
static const char *button_source = "none";

bool acpi_ready(void)
{
    return loaded;
}

/* ---- interrupts ---- */

/* The flags of the SCI. An interrupt source override of the MADT for the
 * ISA interrupt of the SCI gives its trigger mode and polarity. Without
 * one the SCI is level triggered and active low (ACPI 6.5, 5.2.9). */
unsigned acpi_irq_flags(unsigned gsi)
{
    unsigned flags = IRQ_GSI_LEVEL | IRQ_GSI_ACTIVE_LOW;
    uacpi_table table;
    if (uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &table) != UACPI_STATUS_OK)
        return flags;
    const struct acpi_madt *madt = table.ptr;
    const uint8_t *p = (const uint8_t *)madt->entries, *end = (const uint8_t *)madt + madt->hdr.length;
    while (p + sizeof(struct acpi_entry_hdr) <= end) {
        const struct acpi_entry_hdr *e = (const void *)p;
        if (e->length < sizeof *e || p + e->length > end)
            break;
        if (e->type == ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE) {
            const struct acpi_madt_interrupt_source_override *o = (const void *)p;
            if (o->gsi == gsi) {
                unsigned pol = o->flags & ACPI_MADT_POLARITY_MASK, trig = o->flags & ACPI_MADT_TRIGGERING_MASK;
                if (pol == ACPI_MADT_POLARITY_ACTIVE_HIGH)
                    flags &= ~IRQ_GSI_ACTIVE_LOW;
                if (trig == ACPI_MADT_TRIGGERING_EDGE)
                    flags &= ~IRQ_GSI_LEVEL;
            }
        }
        p += e->length;
    }
    uacpi_table_unref(&table);
    return flags;
}

/* ---- the power button ---- */

void acpi_power_button(void)
{
    struct proc *init = proc_init_process();
    klog_info("power button pressed");
    if (init)
        signal_send(init, SIGUSR1);
    else
        klog_warn("no init process receives the power button");
}

static void power_button_work(void *arg)
{
    acpi_power_button();
}

/* The fixed event runs in the interrupt handler of the SCI. */
static uacpi_interrupt_ret fixed_power_button(uacpi_handle ctx)
{
    acpi_defer(power_button_work, NULL);
    return UACPI_INTERRUPT_HANDLED;
}

/* Notifications run in the work thread. 0x80 reports a press. */
static uacpi_status button_notify(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value)
{
    if (value == 0x80)
        acpi_power_button();
    return UACPI_STATUS_OK;
}

static uacpi_iteration_decision button_device(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    if (uacpi_install_notify_handler(node, button_notify, NULL) == UACPI_STATUS_OK)
        button_source = "the power button device of ACPI";
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* ---- the Generic Event Device ---- */

/* Each interrupt of a GED runs its _EVT method with the number of the
 * interrupt. The table is written while acpi_init installs the GEDs and
 * read only afterwards. */
#define MAX_GED_IRQS 8
static struct ged_irq {
    uacpi_namespace_node *device;
    uint32_t gsi;
} ged_irqs[MAX_GED_IRQS];
static unsigned nged;

static void ged_work(void *arg)
{
    struct ged_irq *g = arg;
    uacpi_object *number = uacpi_object_create_integer(g->gsi);
    if (!number)
        return;
    uacpi_object_array args = { &number, 1 };
    uacpi_status st = uacpi_execute(g->device, "_EVT", &args);
    if (st != UACPI_STATUS_OK)
        klog_warn("_EVT(%u): %s", g->gsi, uacpi_status_to_string(st));
    uacpi_object_unref(number);
}

static void ged_interrupt(struct trapframe *tf, void *arg)
{
    acpi_defer(ged_work, arg);
}

static void ged_route(uacpi_namespace_node *device, uint32_t gsi, uint8_t triggering, uint8_t polarity)
{
    if (nged == MAX_GED_IRQS)
        return;
    struct ged_irq *g = &ged_irqs[nged];
    g->device = device;
    g->gsi = gsi;
    unsigned flags = (triggering == UACPI_TRIGGERING_EDGE ? 0 : IRQ_GSI_LEVEL) |
                     (polarity == UACPI_POLARITY_ACTIVE_LOW ? IRQ_GSI_ACTIVE_LOW : 0);
    int r = irq_route_gsi(gsi, flags, ged_interrupt, g);
    if (r < 0) {
        klog_warn("generic event device: cannot route interrupt %u: %d", gsi, r);
        return;
    }
    nged++;
    klog_info("generic event device: interrupt %u, %s triggered", gsi, flags & IRQ_GSI_LEVEL ? "level" : "edge");
}

static uacpi_iteration_decision ged_resource(void *user, uacpi_resource *r)
{
    if (r->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ) {
        for (unsigned i = 0; i < r->extended_irq.num_irqs; i++)
            ged_route(user, r->extended_irq.irqs[i], r->extended_irq.triggering, r->extended_irq.polarity);
    } else if (r->type == UACPI_RESOURCE_TYPE_IRQ) {
        for (unsigned i = 0; i < r->irq.num_irqs; i++)
            ged_route(user, r->irq.irqs[i], r->irq.triggering, r->irq.polarity);
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision ged_device(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    uacpi_for_each_device_resource(node, "_CRS", ged_resource, node);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* ---- initialization ---- */

static bool check(uacpi_status st, const char *what)
{
    if (st == UACPI_STATUS_OK)
        return true;
    klog_error("%s: %s", what, uacpi_status_to_string(st));
    return false;
}

void acpi_init(void)
{
    /* The work thread also serves the power key of a device tree. */
    acpi_start_worker();
    if (!bootinfo.rsdp_phys)
        return;
    acpi_kernel_late();
    /* \_PIC receives the interrupt model after the load and before the
     * initialization of the devices, whose _INI methods may read it. */
    if (!check(uacpi_initialize(0), "uacpi_initialize") ||
        !check(uacpi_namespace_load(), "uacpi_namespace_load") ||
        !check(uacpi_set_interrupt_model(ARCH_ACPI_INTERRUPT_MODEL), "uacpi_set_interrupt_model") ||
        !check(uacpi_namespace_initialize(), "uacpi_namespace_initialize"))
        return;
    uacpi_bool reduced = UACPI_FALSE;
    uacpi_is_platform_reduced_hardware(&reduced);
    if (!reduced)
        check(uacpi_finalize_gpe_initialization(), "uacpi_finalize_gpe_initialization");
    struct acpi_fadt *fadt;
    if (uacpi_table_fadt(&fadt) == UACPI_STATUS_OK)
        reset_register = (fadt->flags & ACPI_RESET_REG_SUP) && fadt->reset_reg.address;
    loaded = true;

    if (!reduced && uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, fixed_power_button, NULL) ==
                        UACPI_STATUS_OK)
        button_source = "the fixed power button event of ACPI";
    uacpi_find_devices("PNP0C0C", button_device, NULL);
    uacpi_find_devices("ACPI0013", ged_device, NULL);
    klog_info("namespace loaded, %s hardware, power button through %s, reset register %s",
              reduced ? "reduced" : "fixed", button_source, reset_register ? "present" : "absent");
}

void acpi_set_button_source(const char *source)
{
    button_source = source;
}

/* ---- power off and reset ---- */

void acpi_power_off(void)
{
    if (!loaded)
        return;
    uacpi_status st = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
    if (st != UACPI_STATUS_OK)
        klog_warn("prepare for S5: %s", uacpi_status_to_string(st));
    arch_irq_save();
    st = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
    klog_error("enter S5: %s", uacpi_status_to_string(st));
}

void acpi_reboot(void)
{
    if (!loaded || !reset_register)
        return;
    uacpi_status st = uacpi_reboot();
    /* The reset takes effect at once on QEMU. A machine that continues
     * receives the methods of the platform. */
    for (volatile long i = 0; i < 100000000L; i++)
        cpu_relax();
    klog_error("the reset register had no effect: %s", uacpi_status_to_string(st));
}

void acpi_describe(struct devinfo *d)
{
    devinfo_prop(d, "acpi_namespace", "%s", loaded ? "loaded by uACPI" : "not loaded");
    devinfo_prop(d, "power_button", "%s", button_source);
}

const char *acpi_reset_method(void)
{
    return loaded && reset_register ? "the reset register of the FADT" : NULL;
}
