#include <arch/init.h>
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/trap.h>
#include <arch/apic.h>
#include <arch/platform.h>
#include <drivers/ps2kbd.h>
#include <drivers/ps2mouse.h>

/* The x86_64 steps of the start-up sequence in init/main.c. */

void arch_init_cpu_boot(void)
{
    gdt_init();
    cpu_init_boot();
}

void arch_init_traps(void)
{
    idt_init();
}

void arch_init_cpu_features(void)
{
    cpu_identify();
}

void arch_init_interrupts(void)
{
    lapic_init();
    ioapic_init();
}

void platform_devices_init(void)
{
    ps2kbd_init();
    ps2mouse_init();
}
