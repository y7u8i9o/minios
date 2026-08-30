#pragma once
#include <kernel.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28

/* Boot CPU tables, using the boot stack as rsp0. */
void gdt_init(void);
/* Build and load the tables of CPU id on the calling CPU. */
void gdt_init_cpu(unsigned id, uintptr_t rsp0);
/* Set the stack used when an interrupt arrives from ring 3. */
void tss_set_rsp0(uintptr_t rsp0);
