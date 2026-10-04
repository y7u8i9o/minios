#pragma once
#include <kernel.h>

/* /dev/devices (docs/design/sysinfo.md): a text description of the machine
 * and its devices, made when the node is opened. The text is a sequence of
 * nodes. A node starts with a line "@PATH<TAB>TITLE" and continues with
 * lines "KEY<TAB>VALUE" for its properties. PATH is a list of names
 * separated by '/'. The node "pci/00:02.0" is a child of the node "pci".
 * Keys are lower case words joined by '_', and their meaning is stable.
 * Titles and values are English text for people.
 *
 * Each subsystem writes its part with the functions below. The writer
 * grows its buffer itself. */
struct devinfo;

/* Start the node path with the given title. A tab or a newline in the
 * title or the path is replaced by a space. */
void devinfo_node(struct devinfo *d, const char *path, const char *title_fmt, ...) __printf(3, 4);
/* Add a property to the current node. */
void devinfo_prop(struct devinfo *d, const char *key, const char *fmt, ...) __printf(3, 4);
/* Append word to the list in buf of size bytes, after sep when the list
 * is not empty. The list is truncated when buf is full. */
void devinfo_append(char *buf, size_t size, const char *sep, const char *word);
/* A size in bytes as text: "512 bytes", "16 KiB" or "1.5 GiB", one
 * decimal rounded down. */
void devinfo_format_size(char *buf, size_t size, uint64_t bytes);
/* A property with a size in bytes, written as the number of bytes and a
 * rounded size in KiB, MiB, GiB or TiB. */
void devinfo_size(struct devinfo *d, const char *key, uint64_t bytes);

/* Copy the SMBIOS tables and register /dev/devices. Called after
 * vmm_init and slab_init. */
void devinfo_init(void);
/* Copy the SMBIOS structure table from the firmware (drivers/smbios.c). */
void smbios_init(void);
/* Read the list of ACPI tables from the RSDP (drivers/acpi_tables.c). */
void acpi_tables_init(void);
/* The node "firmware/acpi" with a child for each table. */
void acpi_tables_describe(struct devinfo *d);


/* The parts, in the order of the text. Each one writes its category node
 * and the nodes below it. */
void arch_describe(struct devinfo *d);          /* "cpu", "platform", "firmware/acpi" or "firmware/devicetree" */
void smbios_describe(struct devinfo *d);        /* "firmware/smbios" */
void pci_describe(struct devinfo *d);           /* "pci" */
void usb_describe(struct devinfo *d);           /* "usb" */
void input_describe(struct devinfo *d);         /* "input" */
void block_describe(struct devinfo *d);         /* "storage" */
void fbdev_describe(struct devinfo *d);         /* "display" */
void pcm_describe(struct devinfo *d);           /* "audio" */
void netif_describe(struct devinfo *d);         /* "network" */
