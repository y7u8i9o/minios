#pragma once

/* Identification of the architecture (docs/design/arch.md). The machine
 * name is reported by uname; the ELF machine number is the only e_machine
 * that the ELF loader accepts. */
#define ARCH_MACHINE_NAME "x86_64"
#define ARCH_ELF_MACHINE  62    /* EM_X86_64 */
