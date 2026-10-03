#pragma once
/* The host build of the libgui tests has no message catalogues: the
 * functions of <libintl.h> return their message. */
#define gettext(s) ((char *)(s))
#define dgettext(d, s) ((char *)(s))
#define ngettext(s, p, n) ((char *)((n) == 1 ? (s) : (p)))
#define dngettext(d, s, p, n) ((char *)((n) == 1 ? (s) : (p)))
#define textdomain(d) ((char *)(d))
#define bindtextdomain(d, dir) ((char *)(dir))
