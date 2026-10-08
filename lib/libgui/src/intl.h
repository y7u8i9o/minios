#pragma once
/* The translated strings of libgui itself. tools/xgettext.py extracts
 * them into the libgui domain (docs/design/i18n.md). */
#include <libintl.h>

#define _(s) dgettext("libgui", s)
#define N_(s) s
#undef ngettext
#define ngettext(s, p, n) dngettext("libgui", s, p, n)
