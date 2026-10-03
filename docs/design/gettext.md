# Message catalogues

The C library translates messages with the interface of GNU gettext:
`gettext`, `dgettext`, `dcgettext`, `ngettext`, `dngettext`, `dcngettext`,
`textdomain`, `bindtextdomain` and `bind_textdomain_codeset`, declared in
`<libintl.h>` and implemented in `libc/src/locale/gettext.c`. The catalogues
are GNU `.mo` files, compiled during the build from the `.po` sources of
`user/po`.

## Sources and the build

The translations of a domain are in `user/po/DOMAIN/LL.po`, one file per
language, for example `user/po/gettexttest/fr.po`. The build compiles each
file with the host tool `msgfmt` (`tools/msgfmt/msgfmt.c`, built as
`build/host/msgfmt`) into `/usr/share/locale/LL/LC_MESSAGES/DOMAIN.mo`. The
plan named a Python program for this step. The build runs no Python, so
`msgfmt` is a host program in C like `mkfs` and `pkgsign`, and no compiled
catalogue is checked in.

`msgfmt` reads `msgctxt`, `msgid`, `msgid_plural`, `msgstr` and
`msgstr[N]` with continued strings and the escapes `\n`, `\t`, `\r`, `\"`
and `\\`. It leaves out entries marked `fuzzy` and entries without a
translation, except the header. It writes the originals sorted by `strcmp`
with their translations and no hash table. A message with a context is
stored as the context, the byte 4 and the message, as in GNU gettext. A
plural message is stored as the singular, a zero byte and the plural, and
its translation as the forms separated by zero bytes.

`tools/xgettext.py` extracts the arguments of `_("...")`, `N_("...")`,
`gettext("...")`, `ngettext("...", "...", n)` and `C_("context", "...")`
from C files into a template:

    tools/xgettext.py -o user/po/DOMAIN/DOMAIN.pot FILES.c

It joins adjacent string literals and lists the file and line of every
occurrence. The template is the start of a new `LL.po` file.

## Lookup

`dcngettext` selects the domain (the current one when the argument is
NULL), then the languages, then the catalogues. The languages are the
entries of `LANGUAGE`, separated by colons, or the name of the locale of
the category, LC_MESSAGES by default. When that locale is `C` or `C.UTF-8`
nothing is translated and `LANGUAGE` is ignored, as in GNU gettext. For an
entry such as `ru_RU.UTF-8` the catalogues of `ru_RU.UTF-8`, `ru_RU` and
`ru` are tried in that order. The first catalogue that contains a
nonempty translation of the message gives the result. Without a
translation the result is the message, or for a plural message with
n other than 1 the plural.

A catalogue is read into memory on first use and remains there with its
plural rule. The catalogue of a domain is in the directory that
`bindtextdomain` set, `/usr/share/locale` by default. A missing file is
remembered as missing. A lock protects the bindings, the current domain
and the list of catalogues. The locale of `uselocale` selects the
language of a thread.

The table of originals is searched with a binary search. The translation
of the empty message is the header of the catalogue. Its `Plural-Forms`
line gives `nplurals` and a C expression in `n`, which a recursive descent
parser turns into a tree of at most 64 nodes. The parser accepts `?:`,
`||`, `&&`, `==`, `!=`, `<`, `>`, `<=`, `>=`, `+`, `-`, `*`, `/`, `%`, `!`,
parentheses, `n` and decimal numbers. A catalogue without the line, or with
an expression that does not parse, uses the rule of English, `n != 1`. An
index at or above `nplurals` selects the first form.

## Translations

The translations of the desktop programs are written for French, Spanish,
Russian, Simplified Chinese and Japanese without a native speaker. A
native speaker should review them before a release.

## Tests

The boot test `gettext` runs `/bin/gettexttest` with the catalogues of
the domain `gettexttest` in French and Russian. It checks translated and
missing messages, the French plural for 0, 1 and 2, the three Russian
forms for ten numbers, a message with a context, the omission of fuzzy and
untranslated entries, continued strings, `LANGUAGE` before LC_MESSAGES and
its fallback to the next language, an entry of `LANGUAGE` with a territory
and a codeset, the C locale, `textdomain`, `dgettext`, `dngettext`,
`dcgettext`, `bindtextdomain` and `uselocale` in a second thread.
