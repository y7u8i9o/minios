#pragma once
/* Message catalogues in the GNU .mo format (docs/design/gettext.md). A
 * catalogue of a domain is /usr/share/locale/LL/LC_MESSAGES/DOMAIN.mo, or
 * is in the directory that bindtextdomain names. */
#include <locale.h>

char *gettext(const char *msgid);
char *dgettext(const char *domain, const char *msgid);
char *dcgettext(const char *domain, const char *msgid, int category);
char *ngettext(const char *msgid, const char *msgid_plural, unsigned long n);
char *dngettext(const char *domain, const char *msgid, const char *msgid_plural, unsigned long n);
char *dcngettext(const char *domain, const char *msgid, const char *msgid_plural, unsigned long n, int category);
char *textdomain(const char *domain);
char *bindtextdomain(const char *domain, const char *dirname);
char *bind_textdomain_codeset(const char *domain, const char *codeset);
