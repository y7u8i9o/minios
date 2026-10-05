/* The x86_64 part of /dev/devices (docs/design/sysinfo.md): the processor
 * from CPUID and the devices of the q35 PC that are outside PCI. */
#include <drivers/devinfo.h>
#include <drivers/acpi.h>
#include <arch/cpu.h>
#include <drivers/pci.h>

void arch_describe(struct devinfo *d)
{
    cpu_describe(d);
    /* The host bridge names the chipset: 8086:29c0 is the Q35 (ICH9) of
     * QEMU q35, 8086:1237 the 440FX of QEMU pc. */
    const char *chipset = pci_find(0x8086, 0x29c0) ? "Intel Q35 with ICH9"
                        : pci_find(0x8086, 0x1237) ? "Intel 440FX with PIIX3" : "unknown";
    devinfo_node(d, "platform", "PC, %s", chipset);
    devinfo_prop(d, "machine", "PC");
    devinfo_prop(d, "chipset", "%s", chipset);
    devinfo_prop(d, "description_source", "fixed addresses of the q35 PC");
    devinfo_prop(d, "interrupt_controller", "local APIC and I/O APIC");
    apic_describe(d);
    devinfo_prop(d, "serial_console", "COM1, I/O port 0x3f8");
    devinfo_prop(d, "keyboard_controller", "8042, I/O ports 0x60 and 0x64");
    devinfo_prop(d, "real_time_clock", "CMOS, I/O ports 0x70 and 0x71");
    devinfo_prop(d, "power_off", "%s", acpi_ready() ? "sleep state S5 of ACPI" : "ACPI PM1a control, I/O port 0x604 (q35)");
    devinfo_prop(d, "reboot", "%s", acpi_reset_method() ? acpi_reset_method() : "8042 reset line");
    acpi_describe(d);
}
