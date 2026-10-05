# binutils

The package `binutils` contains the programs that create, read and change
ELF files and archives: `ar`, `as`, `ld`, `nm`, `size`, `strings`,
`readelf`, `objdump`, `objcopy`, `strip`, `addr2line` and `ranlib`. `as`
and `ld` run the assembler and the linker of tcc (`tcc.md`). The other
programs are written for minios. Their output equals the output of the GNU
binutils 2.47 for the options that minios implements. The programs read
ELF64 little-endian files of x86_64 and aarch64 and System V archives of
such files. Other formats give the message "file format not recognized".
`strings` reads any file. A disassembler for `objdump -d` is planned for
release 0.8 (`docs/plan/roadmap.md`).

## Sources

| Path | Contents |
|---|---|
| `lib/libc/include/elf.h` | the ELF types and constants (`libc.md`) |
| `lib/libc/include/minios/elffile.h`, `lib/libc/src/elffile.c` | the reader of ELF64 files in memory, with the names of the constants as GNU prints them |
| `lib/libc/include/getopt.h`, `lib/libc/src/unistd/getopt_long.c` | `getopt_long` with the behaviour of the GNU C library |
| `user/binutils/lib/` | messages, memory, files, archives, the iteration over the objects of a file |
| `user/binutils/elfdump.c` | the relocations, the dynamic section and the symbol versions for `readelf` and `objdump` |
| `user/binutils/elfrewrite.c` | the ELF rewriter of `objcopy` and `strip` |
| `user/binutils/PROGRAM.c` | one file per program |
| `user/binutils/tests/` | the comparison with the GNU binutils |

`user/Makefile` links each program from its file, the files of
`user/binutils/lib/` and, for four programs, `elfdump.c` or
`elfrewrite.c`. The libc provides the reader and `getopt_long`.

## Shared code

`bu_for_each_object` reads a file. For an ELF file it calls the function
of the program once. For an archive it calls the function for every ELF
member, after an optional function for the archive itself. An optional
third function receives the members that are no ELF files. `readelf`
prints the heading of such a member there.

`archive_parse` builds the list of members of an archive. Each member
receives a copy of its contents. A member starts at an even offset of the
archive, and the ELF structures need an alignment of 8 bytes. With the
copy, every program reads aligned data. `archive_write` writes an archive in
the format of GNU ar. The symbol index `/` comes first, the name table
`//` second. The index lists the defined global, weak and unique symbols
of the ELF relocatable members in the order of the members and of their
symbol tables. The index has the date `index_date` of the archive, 0 by
default. `ranlib` sets the current time. GNU ar pads the index with a zero
byte and the name table with a newline to an even size, and both pads
count in the size of the member. `archive_write` writes the same bytes.

The programs parse their options with `getopt_long`. Options and file
names may appear in any order, and a unique prefix selects a long option.
`objcopy` and `strip` give the letter `-S` different meanings, as GNU
does. `-S` is `--strip-all` for `objcopy` and `--strip-debug` for
`strip`. Each of the two programs therefore maps its letters to the codes
of `elfrewrite_apply`.

## Programs

- `ar` maintains archives (`artar.md`). Since 2026-10-05 every rewrite
  writes the symbol index, unless the modifier `S` is given. The
  operation `s` writes the index alone.
- `nm` prints the symbol tables in the formats bsd, sysv, posix and
  just-symbols, with the GNU symbol type letters, the sorting options,
  `-D` for the dynamic symbols and `-s` for the index of an archive.
  `nm` sorts names with `strcoll` after `setlocale(LC_ALL, "")`, as GNU
  does.
- `size` prints the sizes of the sections in the formats berkeley, sysv
  and gnu, with totals and the radix options.
- `strings` prints the printable strings of a file in the encodings `s`,
  `S`, `b`, `l`, `B` and `L`. `-NUMBER` is the short form of `-n NUMBER`.
- `readelf` prints the file header, the program headers, the section
  headers, the section groups, the symbol tables, the relocations, the
  dynamic section, the notes, the symbol versions, the attributes of
  aarch64, the histogram of the hash tables, the GOT and the hex and string
  dumps of sections. minios `readelf` prints the wide format of `-W`
  always.
- `objdump` prints the archive headers, the file header, the section
  headers, the private headers, the symbol tables, the relocations and
  the contents of sections. `-d`, `-D` and `-S` give the message that
  disassembly is not available.
- `objcopy` copies an ELF file or the ELF members of an archive. It
  removes or selects sections, strips symbols and debug information, adds
  sections from files and writes the loadable contents with `-O binary`.
- `strip` removes symbols and sections. Without an option for symbols it
  removes all symbols, as `-s` does. Executables and shared objects
  retain their program headers and the file offsets of all allocated
  sections. In relocatable objects the symbols that relocations use
  remain, and the relocations receive the new symbol indices.
- `addr2line` translates addresses into files, lines and functions. It
  reads the DWARF versions 2 to 5: the line programs of `.debug_line`,
  the functions and inlined functions of `.debug_info`, and the ranges of
  `.debug_ranges` and `.debug_rnglists`. `addr2line` takes the function
  from the symbol table when no DWARF information covers the address. In relocatable
  objects the relocations of the debug sections are applied first.
- `ranlib` writes the symbol index of archives. `-D` writes the date 0
  into the index, and `-t` only changes the date of an existing index.

## Differences from GNU

- The programs read no ELF32 files, no big-endian files and no compressed
  sections (`SHF_COMPRESSED`).
- `nm -D` prints no version suffixes such as `@VER`. The options `-C`,
  `-l`, `-e` and `-X` of `nm` are accepted without effect.
- `strings` accepts `-U` and `-T` without effect.
- `readelf -u` decodes no unwind sections and prints the message of GNU
  for the machine. `readelf -A` decodes the aarch64 subsection
  `aeabi_feature_and_bits` and the generic tags.
- `objcopy` and `strip` change no segments when `-j` or `-R` removes an
  allocated section of an executable. The output of `-O binary` equals
  the output of GNU in that case.
- `addr2line` reads no `.debug_aranges` and selects the unit by its own
  address ranges. The options `-b` and `-C` are not available.
- The messages on the standard error differ from GNU in their wording.

## Tests

`make check-binutils` compiles the programs for the host and compares
their output with the GNU binutils of the cross toolchains on files of
the current build of both architectures (`user/binutils/tests/run.sh`).
The files are an object with debug information, the archive `libedit.a`,
the shared object `libjson.so`, a program with debug information, the
installed `sh` and the kernel. The scripts of the four groups compile
further programs with the GNU toolchain: programs with DWARF 2 to 5 and
inlined functions, versioned shared objects, section groups and notes.
For `objcopy` and `strip` the script compares the section tables without
offsets, the symbol tables, the program headers and the relocations of
the results through GNU `readelf`. It requires byte equality for
`-O binary` and checks that GNU `ld -r` accepts the stripped objects. The
check of 2026-10-05 ran 3852 comparisons without a difference.

The boot case `binutils` (`/etc/tests/binutils.sh`) compiles two files
with `tcc -gdwarf` inside minios and checks each program on the results.
The script prints the number of failed checks last, and the case requires
`binutils: 0 failed`.
A stripped program must run, an object stripped with `--strip-unneeded`
must link, and `addr2line` must find the function and the line of
`twice`. The boot case `libc_ext` checks `getopt_long`.
