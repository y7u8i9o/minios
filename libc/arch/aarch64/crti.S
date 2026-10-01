/* crti.o, linked before every object by tcc. It defines the _init and
 * _fini functions that __libc_start calls in a static program, as empty
 * functions: minios objects put their constructors in .init_array. */
    .section .text
    .global _init
    .type _init, %function
_init:
    ret
    .global _fini
    .type _fini, %function
_fini:
    ret
    .section .note.GNU-stack,"",%progbits
