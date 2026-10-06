# Locales

The C library supports the locales `C` (also named `POSIX`), `C.UTF-8`
and the locales whose files are in `/usr/share/i18n/locales`: `en_US`,
`fr_FR`, `es_ES`, `ru_RU`, `zh_CN` and `ja_JP`. Every locale decodes
UTF-8, `MB_CUR_MAX` is 4, and `nl_langinfo(CODESET)` returns `UTF-8` in
every locale. The code is in `lib/libc/src/locale/` (`locale.c`, `collate.c`,
`locale_impl.h`), with the users of the items in `lib/libc/src/stdio/format.c`,
`lib/libc/src/stdio/scan.c`, `lib/libc/src/stdlib/stdlib.c` and
`lib/libc/src/time/tm.c`.

## Locale files

A locale file has one item per line: a key, a space and a value. A value is
a quoted string, a quoted list separated by semicolons, or a number. The
escapes `\\`, `\"` and `\uXXXX` are recognised in quoted values. Lines that
start with `#` and lines in upper case are comments. Section titles such as
`LC_TIME` are therefore comments. The files are in
`user/share/i18n/locales/` and the build copies them to
`/usr/share/i18n/locales/`. The values follow CLDR.

| Category | Keys |
|---|---|
| LC_NUMERIC | `decimal_point`, `thousands_sep`, `grouping` (group sizes from the right, such as "3") |
| LC_MONETARY | `int_curr_symbol`, `currency_symbol`, `mon_decimal_point`, `mon_thousands_sep`, `mon_grouping`, `positive_sign`, `negative_sign`, `int_frac_digits`, `frac_digits`, `p_cs_precedes`, `p_sep_by_space`, `n_cs_precedes`, `n_sep_by_space`, `p_sign_posn`, `n_sign_posn` |
| LC_TIME | `abday`, `day`, `abmon`, `mon`, `alt_mon`, `ab_alt_mon`, `am_pm`, `d_t_fmt`, `d_fmt`, `t_fmt`, `t_fmt_ampm`, `date_fmt`, `first_weekday` |
| LC_MESSAGES | `yesexpr`, `noexpr`, `yesstr`, `nostr`, `language_name`, `territory_name` |
| LC_COLLATE | `collate` (`latin` or `codepoint`), `collate_after` |

`mon` contains the month names used with a day. `alt_mon` and `ab_alt_mon`
contain the names used without a day, which differ in Russian (genitive
"января" and nominative "январь"). A missing `alt_mon` is `mon`. Any other
missing item takes the value of the C locale. `first_weekday` is the first
day of the week as one digit, 0 for Sunday to 6 for Saturday, which
`nl_langinfo(_NL_FIRST_WEEKDAY)` returns. It is 0 in the C locale, in
`en_US` and in `ja_JP`, and 1 in the other locales. The calendar of the
panel orders its columns by it (`shell.md`). `language_name` and
`territory_name` name the locale in its own language for the Settings
program.

## Names and setlocale

`setlocale` accepts `C`, `POSIX`, `C.UTF-8` and `C.utf8`, and for a file
`ll_CC` the names `ll_CC.UTF-8`, `ll_CC.utf8`, `ll_CC` and `ll`. The form
`ll` selects the first file of that language in the order of the names. A
codeset other than UTF-8 is refused. The canonical name of a locale file is
`ll_CC.UTF-8`, and `setlocale` returns that name. For the empty name every
category takes `LC_ALL`, then its own variable `LC_*`, then `LANG`, then
`C`. When the categories differ, `setlocale(LC_ALL, NULL)` returns the list
`LC_COLLATE=...;LC_CTYPE=...;LC_MONETARY=...;LC_NUMERIC=...;LC_TIME=...;LC_MESSAGES=...`,
and `setlocale(LC_ALL, list)` accepts it. A call that names an unknown
locale returns NULL and changes no category.

`setlocale` reads a file once. The parsed copy, `struct locale_data`, is
stored in a list for the life of the process, and the file text is the
string pool of its items. A lock protects the list and the buffer of
names that `setlocale` returns.

## Locale objects

`locale_t` points to a `struct __locale_struct`, which contains one
`struct locale_data` per category. `newlocale` sets the categories of its
mask in its base, or in a new object initialised to C when the base is
NULL, and returns NULL with `ENOENT` for an unknown name. `duplocale`,
`freelocale` and `uselocale` follow POSIX. The locale of `uselocale` is
stored in the thread control block (`struct pthread`, field `locale`), and
a NULL field selects the global locale. Every function that depends on the
locale uses `__locale_current()`, which returns the locale of the calling
thread. `nl_langinfo_l`, `strcoll_l`, `strxfrm_l`, `strftime_l` and
`strtod_l` take a locale object.

## Numbers

`printf` writes the radix character of LC_NUMERIC in `a`, `e`, `f` and
`g` conversions. The flag `'` inserts the thousands separator into the
integer digits of `d`, `i`, `u`, `f`, `F`, `g` and `G`, following the
group sizes of `grouping`. The C locale has no separator, and the flag has
no effect there. `localeconv` fills its structure from the current locale,
and a numeric item of -1 in a file becomes `CHAR_MAX`.

`strtod` copies the number with the radix character of the locale replaced
by a full stop and parses the copy with the parser of the C locale. In a
locale whose radix character is not a full stop, a full stop ends the
number. The copy has room for 511 bytes. `strtof`, `strtold` and `atof` use
`strtod`. `scanf` accepts the radix character of the locale when it is one
byte.

## Dates

`strftime` takes the day and month names, the strings of `%p` and the
formats of `%c`, `%x`, `%X` and `%r` from LC_TIME. `%P` gives `%p` in lower
case. The modifier `O` selects `alt_mon` and `ab_alt_mon` for `%B`, `%b` and
`%h`, and the modifier `E` is accepted and ignored, because no locale has
an era. The conversions `%G`, `%g` and `%V` give the ISO 8601 week year and
week, `%U` and `%W` the week of the year counted from Sunday and from
Monday, `%k` and `%l` the hour padded with a space, and `%s` the seconds
since the epoch through `mktime`. `struct tm` has no `tm_gmtoff` and no
`tm_zone`, because a larger structure would break programs that pass a
`struct tm` of the old size.

## Collation

The C locale, `C.UTF-8`, `zh_CN` and `ja_JP` have `collate "codepoint"`:
`strcoll` is `strcmp` and `strxfrm` copies. The other locales have
`collate "latin"`. `strxfrm` then builds a sort key of four levels, and
`strcoll` compares the keys of its two strings.

1. The primary level contains one weight per character. A precomposed
   letter of the Latin, Greek and Cyrillic blocks takes the weight of its
   base letter, from the table `uni_bases` that `tools/genunicode.py`
   generates from the canonical decompositions. Letters are folded to
   lower case. Spaces, punctuation and symbols come first, by code point,
   then the digits, then the Latin, Greek and Cyrillic letters, then the
   other characters by code point. Marks have no primary weight.
2. The secondary level contains the first combining mark of each
   character, 0 for none.
3. The tertiary level contains 1 for an upper case letter and 0 otherwise,
   so a lower case word comes first.
4. The last level contains the code points.

Each weight takes four bytes from 2 to 255, and the byte 1 separates the
levels. The key therefore has no zero byte, and `strcmp` orders keys as
`strcoll` orders strings. `collate_after` lists pairs "base letter": in `es_ES`,
"n ñ" makes ñ a letter of its own between n and o. In `ru_RU`, ё has the
primary weight of е and differs at the second level, as in CLDR.
`wcscoll` and `wcsxfrm` build the same keys from wide strings. The
collation does not implement the Unicode Collation Algorithm, the
contractions of other languages, or the pinyin order of Chinese.

## The locale utility

`locale` prints `LANG`, each category and `LC_ALL` in the format of POSIX,
with the names that only `LANG` or `LC_ALL` imply in quotes. `locale -a`
lists `C`, `C.UTF-8`, `POSIX` and the files of `/usr/share/i18n/locales`.
`locale [-k] keyword...` prints the values of the keywords that the
manual page lists.

## Programs

Lua's `os.setlocale` calls `setlocale`, and Lua writes and reads numbers
with the radix character of the locale. awk calls `setlocale(LC_CTYPE, "")`
and leaves LC_NUMERIC at C. sed calls `setlocale(LC_ALL, "")`. Every
program written for minios uses the C locale until it calls `setlocale`.

## Tests

The boot test `locale` runs `/bin/localetest`. It checks the accepted and
refused names, the environment variables and their precedence, composite
names, the decimal point, `%.2f` and the grouping of `%'d` and `%'.2f` in
every locale, `strtod` and `sscanf` with a decimal comma, 13 `strftime`
formats in six locales, the ISO weeks at the year ends, `nl_langinfo` and
`localeconv`, the collation of English, French, Spanish, Russian and C
words together with the order of their `strxfrm` keys, `newlocale`,
`duplocale` and `uselocale` in a second thread, `locale -a`, `locale -k`
and Lua's `os.setlocale`, `os.date` and number output.
