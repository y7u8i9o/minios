# Locales

This plan adds named locales, time zones, message catalogues, keyboard
layouts and input methods. Milestone identifiers use the prefix `L` so that
they do not renumber the development milestones. Each milestone ends with a
boot test and a document in `docs/design/`, and is marked completed here when
its boot tests pass. The work is on the branch `bleeding-edge-locale`.

## 1. Motivation and scope

The C library has a single C locale. `setlocale` accepts only `C`, `POSIX`
and the empty string, and it does not read `LANG` or `LC_*`. `localtime` is
`gmtime`, day and month names are English, the decimal point is always a
full stop, and `strcoll` is `strcmp`. The only keyboard layout is `us`. The
kernel console has fixed US tables. Every string of the user interface is
English. The terminal gives every code point one cell, and the interface font
has no CJK fallback.

The image receives the locales `C`, `C.UTF-8`, `en_US`, `fr_FR`, `es_ES`,
`ru_RU`, `zh_CN` and `ja_JP`, time zones, translated desktop programs in
French, Spanish, Russian, Simplified Chinese and Japanese, the keyboard
layouts `us`, `fr`, `es`, `ru` and `jp`, and input methods for Chinese and
Japanese.

## 2. Fixed decisions

- Every locale uses UTF-8. `C` and `POSIX` also decode UTF-8 in `mbrtowc`.
  `MB_CUR_MAX` is 4.
- A locale is a text file `/usr/share/i18n/locales/<name>` with one `key
  value` pair per line. The C library reads the file when `setlocale`
  selects the locale and stores one parsed copy per loaded locale. The values
  come from CLDR.
- The accepted names are `ll_CC.UTF-8`, `ll_CC.utf8`, `ll_CC` and `ll`. The
  form `ll` selects the first locale of that language. `setlocale(cat, "")`
  uses `LC_ALL`, then `LC_<cat>`, then `LANG`.
- A time zone is a POSIX TZ string or a TZif file. The C library reads `TZ`,
  and without `TZ` it reads `/etc/localtime`. `tools/genzoneinfo.py` writes
  the files in `/usr/share/zoneinfo` from a table of zone names and their
  current POSIX rules. The files are TZif version 2 files without
  transitions, with the rule as the footer. Dates before the last rule change
  of a zone are converted with the current rule.
- Message catalogues use the GNU `.mo` format in
  `/usr/share/locale/<ll>/LC_MESSAGES/<domain>.mo`. `tools/msgfmt.py`
  compiles the sources `user/po/<domain>/<ll>.po`. The translations were
  written without a native speaker and require a review.
- Collation compares Latin and Cyrillic text in three levels: base letter,
  accents, case. Other scripts compare by code point.
- CJK text uses Droid Sans Fallback Full (Apache 2.0) as a second fallback
  font, fetched into `third_party/fonts` by a script.

## 3. Milestones

### L0. Unicode character data (completed 2026-10-02)

- `tools/fetch_unicode.sh` downloads `UnicodeData.txt`,
  `EastAsianWidth.txt` and `DerivedCoreProperties.txt` of Unicode 16.0 into
  `third_party/unicode`.
- `tools/genunicode.py` generates range tables into
  `libc/src/wchar/unidata.h`. `wcwidth`, the `isw*` classes, `towupper` and
  `towlower` use the generated tables.
- The terminal uses `wcwidth`. Wide characters occupy two cells, and
  combining marks attach to the previous cell.
- The boot test is `unicode_data`, and the design document is
  `docs/design/unicode.md`.

The tables contain about 3,800 ranges. `iswspace` no longer contains the
no-break space U+00A0, and `libc_ext` expects that. The cases
`unicode_data`, `libc_ext`, `gui_term`, `gui_term_scale2`, `lineedit`,
`lineedit_screen`, `awk`, `sed` and `lua` pass.

### L1. Locale core (completed 2026-10-02)

- A parser for locale files in `libc/src/locale/`, `<langinfo.h>` with
  `nl_langinfo`, and `locale_t` with `newlocale`, `uselocale`, `freelocale`
  and `duplocale`. `strtod_l`, `strcoll_l`, `strxfrm` and `strftime_l`.
- `printf`, `scanf`, `strtod` and `localeconv` take the decimal point from
  LC_NUMERIC. `printf` groups digits for the `'` flag.
- `strftime` takes names and formats from LC_TIME, supports `%OB` for the
  nominative month names of Russian, and adds `%G %g %V %U %W %s %k %l %P`.
- LC_COLLATE provides the collation of the fixed decisions, and LC_MESSAGES
  provides `yesexpr` and `noexpr`.
- Locale files for `en_US`, `fr_FR`, `es_ES`, `ru_RU`, `zh_CN` and `ja_JP`,
  and the `locale` utility with a manual page.
- The boot test is `locale`, and the design document is
  `docs/design/locale.md`.

The locale files are text files that libc parses when `setlocale` selects
them. `collate` and `collate_after` select the collation per locale. The
`uselocale` locale is stored in `struct pthread`. The size of `struct tm` is
unchanged. The cases `locale`, `libc`, `libc_ext`, `float`, `time`, `awk`, `sed`,
`lua`, `unicode_data` and `calculator` pass.

### L2. Time zones (completed 2026-10-02)

- A parser for POSIX TZ strings, a TZif reader for versions 1 to 3, and
  `tzset`. `localtime`, `mktime`, `tzname`, `timezone` and `daylight` follow
  daylight saving time. The C library reads `/etc/localtime` again when its
  modification time changes.
- `tools/genzoneinfo.py` and a table of about 40 zones.
- `date`, `ls`, `cal`, the panel clock, the clock program, Files, Settings and
  the screenshot file names show local time.
- The boot test is `timezone`, and `docs/design/time.md` describes the zones.

The zone files are generated into `user/share/zoneinfo` and checked in, as
the build runs no Python. A value of `TZ` is a rule when it parses as one,
because rules such as `M10.5.0/3` contain slashes. The cases `timezone`,
`time`, `libc_ext`, `locale`, `lua`, `comp_panel`, `gui_images` and
`gui_settings` pass.

### L3. Message catalogues (completed 2026-10-02)

- `<libintl.h>` with `gettext`, `dgettext`, `dcgettext`, `ngettext`,
  `dngettext`, `textdomain` and `bindtextdomain`. The reader maps the `.mo`
  file and searches its sorted table. An evaluator computes the C expression
  of the `Plural-Forms` header. `LANGUAGE` takes precedence over
  LC_MESSAGES.
- `tools/msgfmt.py` and `tools/xgettext.py`.
- The boot test is `gettext`, and the design document is
  `docs/design/gettext.md`.

`msgfmt` became a host program in C (`tools/msgfmt/msgfmt.c`), because the
build runs no Python. The build compiles every `user/po/DOMAIN/LL.po`.
`tools/xgettext.py` remains a Python program for maintainers. The cases
`gettext` and `locale` pass.

### L4. Fonts and translated desktop (completed 2026-10-03)

- The font fallback is a chain: DejaVu Sans, DejaVu Sans Mono, Droid Sans
  Fallback.
- `app_create` calls `setlocale(LC_ALL, "")` and binds the `libgui` domain.
- The strings of libgui, the panel and launcher, the desktop, Settings,
  Files, the terminal, sysmon, logview, gedit, mandel, player and screenshot
  are marked with `_()`. Launcher titles are translated through a `launcher`
  domain.
- Catalogues for `fr`, `es`, `ru`, `zh_CN` and `ja`.
- Files and sysmon show dates with `%x` and `%X` and numbers with the decimal
  separator of the locale.
- The boot test is `gui_locale`, and the design document is
  `docs/design/i18n.md`.

The CJK font is in `third_party/droidfallback` and is read on first use,
because libfont reads a font file into memory and the file has 4 MB.
screenshot has no graphical interface and no catalogue. The work found a
race in libgui: a window destroyed with a pending frame callback received
the callback's event after it was freed, and Files died in about one run
of `gui_files` in three. That fix is a separate commit. The cases
`gui_locale`, `gui_settings`, `gui_desktop`, `gui_files`, `gui_term`,
`gui_term_scale2`, `comp_panel`, `gui_wm`, `gui_sysmon`, `gui_logview`,
`gui_editor`, `gui_mandel`, `audio_player`, `gui_images`, `gui_tools`,
`prof_gui`, `gui_widgets` and `gui_controls` pass, and `make check` for
libgui passes.

### L5. Keyboard layouts

- A text source format `user/share/keymaps/src/<name>.kmap`, compiled by
  `tools/genkeymap` into the format MKM2 with the levels plain, Shift, AltGr
  and AltGr+Shift, dead keys and compositions, and up to two groups with a
  switch key.
- The layouts `us`, `fr`, `es`, `ru` (groups `us` and `ru`, switched with
  Alt+Shift) and `jp`.
- libgui and the compositor support levels, dead keys and groups.
- The kernel console takes a keymap through an `ioctl`, emits UTF-8 and
  decodes UTF-8 in `fbcon`. The `loadkeys` utility, and init applies the
  `keymap` setting at boot.
- The boot test is `keymap`, and the design document is
  `docs/design/keymaps.md`.

### L6. Input methods for Chinese and Japanese

- A `set_cursor_rectangle` request in `protocol/text.xml`, sent by the text
  widgets of libgui.
- In the compositor, a Japanese engine converts romaji to hiragana or
  katakana and offers kanji by reading, one character at a time. A Chinese
  engine converts pinyin to hanzi candidates ordered by the levels of the
  table of general standard characters. The candidates come from Unihan. A
  candidate window appears below the cursor rectangle.
- Super+Space cycles the layouts and engines, and the panel shows the
  current one.
- `tools/fetch_unihan.sh` and `tools/genime.py`.
- The boot test is `ime`, and the design document is `docs/design/ime.md`.

### L7. Region and language settings

- A Region and language page in Settings with the language, the formats, the
  time zone and the keyboard layouts.
- The page writes `lang` and `formats` to `desktop.conf` and links
  `/etc/localtime` to the selected zone. `startgui` and `/etc/profile`
  export `LANG` and the format categories.
- The boot test is `gui_region`, and `docs/design/desktop.md` describes the
  page.
