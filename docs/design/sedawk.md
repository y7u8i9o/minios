# sed and awk

`/bin/sed` is the sed of FreeBSD and `/bin/awk` is the One True AWK of
Brian Kernighan, both compiled unmodified from `third_party/`. The port
consists of the libc functions the sources needed, the build rules, two
manual pages and two boot tests.

## Sources

`third_party/sed/src/` contains the six source files, the manual page and
the `POSIX` notes of `usr.bin/sed` from the FreeBSD source tree, and
`third_party/awk/src/` the sources, the yacc grammar, `maketab.c` and
the manual page of `contrib/one-true-awk` from the same tree. Both
`NOTICE` files record the mirror commit, the licence and the download
date. `tools/fetch_sedawk.sh` downloads them again; a later version is
dropped in by running it. The vendored files are exempt from the minios
line count and naming conventions.

## Build

`user/Makefile` compiles the sed sources into `build/user/sed/` and
links `/bin/sed` against libc alone. For awk it runs `$(YACC) -d` on
`awkgram.y` into `build/user/awk/gen/`, compiles `maketab.c` with the
host compiler, runs it on the generated `awkgram.tab.h` to produce
`proctab.c`, and compiles the awk sources with the generated directory
on the include path. `YACC` defaults to `yacc` in `toolchain.mk`;
Berkeley yacc and bison both work. The vendored sources compile with
the user flags plus `-Wno-unused-but-set-variable -Wno-sign-compare
-Wno-implicit-fallthrough -Wno-missing-field-initializers`; the
generated parser also has unused symbols suppressed.

## libc additions

Compiling the sources reported what was missing. Everything below is
available to every program and documented in `libc.md`.

- `err.h`: `err`, `errc`, `errx`, `warn`, `warnc`, `warnx` and their
  `v` forms. They print the program name, which `__libc_start` now
  records from `argv[0]` and `getprogname` returns.
- `unistd.h`: `getopt` with `optarg`, `optind`, `opterr`, `optopt` and
  `optreset`; `readv` and `writev` (`sys/uio.h`), performed as loops of
  `read` and `write`; `fchown` and `chown`.
- `sys/stat.h`: the permission bit macros, `DEFFILEMODE`, `lstat` (the
  same as `stat` until symbolic links were added on 2026-09-30, a system
  call since then), `fchmod` and `chmod`.
  Permission bits were stored and ignored by the kernel and could not be
  changed, so the four change functions returned 0 without effect. They
  are system calls since U1 of the multiuser plan (`users.md`).
- `stdio.h`: `scanf`, `fscanf`, `sscanf` and their `v` forms
  (`lib/libc/src/stdio/scan.c`), `asprintf`, `vasprintf`, `FOPEN_MAX`.
- `stdlib.h`: `bsearch`, `random`, `srandom`, `mbtowc`, `wctomb`,
  `mblen`, `getprogname`, `setprogname`.
- `wctype.h`: the `isw*` classes, `towupper`, `towlower`, `wctype` and
  `iswctype`, for ASCII, Latin-1, Latin Extended-A and B, Greek and
  Cyrillic.
- `strings.h`: `strcasecmp`, `strncasecmp`, `ffs`. `libgen.h`:
  `basename`, `dirname`. `limits.h`: `PATH_MAX`, `LINE_MAX`,
  `_POSIX2_LINE_MAX` and the other POSIX limits over the compiler's
  integer limits. `sys/cdefs.h` and `sys/param.h`: the BSD macros
  (`__dead2`, `__unreachable`, `MIN`, `MAX`, `nitems`). `sys/types.h`:
  `u_char`, `u_int`, `u_long` and the other BSD names.
- `regexec` with `REG_STARTEND` accepted `nmatch` 0 as an error. The
  BSD definition reads `pmatch[0]` as input regardless of `nmatch`, and
  sed matches addresses that way; the check was removed.

## Behaviour

Both programs behave as on FreeBSD, which differs from GNU sed and gawk
in these points: `sed -i` takes a mandatory backup extension (`-i ''`
for none); a label after `b`, `t`, `T` or `:` extends to the end of the
line, so branch commands are separated with `-e` or newlines; awk has
no `gensub`, `--re-interval` or `length` of an array before the second
edition (this release has it), and `printf %c` with a number prints the
UTF-8 encoding of that code point. `awk --version` prints the release
date.

## Tests

`tests/cases/sed` runs `/etc/tests/sed.sh`: addresses, ranges, `s` with
flags and back references in both syntaxes, `y`, `a`, `i`, `c`, groups,
`q`, `=`, `l`, `t` with labels, `N`, reversal through the spare buffer, in place
editing with and without a backup, `-f`, `w` and `r`.
`tests/cases/awk` runs `/etc/tests/awk.sh`: fields and separators,
patterns, `BEGIN` and `END`, `printf`, the string, array, arithmetic and
time functions, user functions, loops, string and number comparison,
`getline` from files and commands, output to files and pipes,
`ENVIRON`, `-v`, `ARGV`, paragraph mode, UTF-8 `substr`, `exit`,
`system` and `-f`. Both scripts print a `FAIL` line per failing check,
which the case rejects. `tests/cases/libc_ext` checks the libc
additions directly.
