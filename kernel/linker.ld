/* Higher half kernel layout. .ksyms is deliberately the last section so the
 * second link pass, which fills it, does not move any other symbol. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R X */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* RW  */
    ksyms  PT_LOAD FLAGS(4);   /* R   */
}

SECTIONS
{
    . = 0xffffffff80000000;
    __kernel_start = .;

    .text : {
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    } :text

    . = ALIGN(0x1000);
    .rodata : {
        __rodata_start = .;
        *(.rodata .rodata.*)
        . = ALIGN(16);
        __ktests_start = .;
        KEEP(*(.ktests))
        __ktests_end = .;
    } :rodata

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :rodata
    __rodata_end = .;

    . = ALIGN(0x1000);
    .data : {
        __data_start = .;
        *(.data .data.*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data
    __data_end = .;

    . = ALIGN(0x1000);
    .ksyms : {
        __ksyms_start = .;
        KEEP(*(.ksyms))
        __ksyms_end = .;
    } :ksyms

    . = ALIGN(0x1000);
    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note*)
        *(.comment)
    }
}
