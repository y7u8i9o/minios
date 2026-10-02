#pragma once
/* Translated messages for the programs of the desktop
 * (docs/design/i18n.md).  A program calls textdomain with its domain after
 * app_create, which sets the locale of the environment, and marks its
 * strings with _() or, in static tables, with N_() and translates them with
 * _() where they are shown. */
#include <libintl.h>

#define _(s) gettext(s)
#define N_(s) (s)
