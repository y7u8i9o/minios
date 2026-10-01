#pragma once
#include <kernel.h>
/* HIGHER_HALF_BASE, KERNEL_VBASE and the user and kernel regions. */
#include <arch/memlayout.h>

/* Higher half direct map offset, set from the Limine HHDM response. */
extern uintptr_t hhdm_offset;

static inline void *phys_to_virt(uintptr_t pa)
{
    return (void *)(pa + hhdm_offset);
}

static inline uintptr_t virt_to_phys(const void *va)
{
    return (uintptr_t)va - hhdm_offset;
}

#define P2V(pa) phys_to_virt((uintptr_t)(pa))
#define V2P(va) virt_to_phys((const void *)(va))

/* Linker script symbols. */
extern char __kernel_start[], __kernel_end[];
extern char __text_start[], __text_end[];
