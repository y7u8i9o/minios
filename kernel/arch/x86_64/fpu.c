#include <arch/fpu.h>
#include <lib/string.h>

#define CR0_MP (1UL << 1)
#define CR0_EM (1UL << 2)
#define CR0_TS (1UL << 3)
#define CR4_OSFXSR (1UL << 9)
#define CR4_OSXMMEXCPT (1UL << 10)

static uint8_t fpu_template[FPU_AREA_SIZE] __attribute__((aligned(16)));
static bool template_ready;

void fpu_init_cpu(void)
{
    uint64_t cr0, cr4;
    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
    cr0 = (cr0 | CR0_MP) & ~(CR0_EM | CR0_TS);
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    __asm__ volatile("movq %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile("movq %0, %%cr4" : : "r"(cr4) : "memory");
}

void fpu_init_state(void *area)
{
    if (!template_ready) {
        uint32_t mxcsr = 0x1f80;
        __asm__ volatile("fninit; ldmxcsr %0" : : "m"(mxcsr));
        fpu_save(fpu_template);
        template_ready = true;
    }
    memcpy(area, fpu_template, FPU_AREA_SIZE);
}
