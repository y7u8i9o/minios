/* L3: message catalogues.  The French and Russian catalogues of the domain
 * gettexttest (user/po/gettexttest), plural forms, contexts, fuzzy and
 * untranslated entries, LANGUAGE, the C locale, domains, bindings and
 * uselocale. */
#include <libintl.h>
#include <locale.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void same(const char *got, const char *want, const char *what)
{
    if (got && strcmp(got, want) == 0)
        return;
    failures++;
    printf("gettexttest: FAIL %s: \"%s\", expected \"%s\"\n", what, got ? got : "(null)", want);
}

static void test_french(void)
{
    setlocale(LC_ALL, "fr_FR.UTF-8");
    same(gettext("Open"), "Ouvrir", "a translated message");
    same(gettext("Save as..."), "Enregistrer sous...", "a message with dots");
    same(ngettext("%d file", "%d files", 0), "%d fichier", "French plural for 0");
    same(ngettext("%d file", "%d files", 1), "%d fichier", "French plural for 1");
    same(ngettext("%d file", "%d files", 2), "%d fichiers", "French plural for 2");
    same(gettext("menu\004File"), "Fichier", "a message with a context");
    same(gettext("Quit"), "Quit", "a fuzzy entry is left out");
    same(gettext("Untranslated"), "Untranslated", "an untranslated entry");
    same(gettext("Multiline"), "Plusieurs\nlignes", "continued strings");
    same(gettext("Nothing"), "Nothing", "a missing message");
    same(ngettext("%d dog", "%d dogs", 2), "%d dogs", "a missing plural message");
}

static void test_russian(void)
{
    static const struct { unsigned long n; const char *want; } forms[] = {
        { 1, "%d файл" }, { 2, "%d файла" }, { 4, "%d файла" }, { 5, "%d файлов" }, { 11, "%d файлов" },
        { 12, "%d файлов" }, { 21, "%d файл" }, { 22, "%d файла" }, { 111, "%d файлов" }, { 1001, "%d файл" },
    };
    setlocale(LC_ALL, "ru_RU.UTF-8");
    same(gettext("Open"), "Открыть", "Russian message");
    char what[48];
    for (size_t i = 0; i < sizeof forms / sizeof forms[0]; i++) {
        snprintf(what, sizeof what, "Russian plural for %lu", forms[i].n);
        same(ngettext("%d file", "%d files", forms[i].n), forms[i].want, what);
    }
    same(gettext("Save as..."), "Save as...", "a message missing in Russian");
}

static void test_language(void)
{
    setlocale(LC_ALL, "fr_FR.UTF-8");
    setenv("LANGUAGE", "ru:fr", 1);
    same(gettext("Open"), "Открыть", "LANGUAGE before LC_MESSAGES");
    same(gettext("Save as..."), "Enregistrer sous...", "the next language of LANGUAGE");
    setenv("LANGUAGE", "ru_RU.UTF-8", 1);
    same(gettext("Open"), "Открыть", "an entry of LANGUAGE with a territory and a codeset");
    setlocale(LC_ALL, "C");
    setenv("LANGUAGE", "fr", 1);
    same(gettext("Open"), "Open", "the C locale translates nothing");
    unsetenv("LANGUAGE");
}

static void test_domains(void)
{
    setlocale(LC_ALL, "fr_FR.UTF-8");
    textdomain("other");
    same(textdomain(NULL), "other", "textdomain");
    same(gettext("Open"), "Open", "a domain without catalogues");
    same(dgettext("gettexttest", "Open"), "Ouvrir", "dgettext");
    same(dngettext("gettexttest", "%d file", "%d files", 3), "%d fichiers", "dngettext");
    same(dcgettext("gettexttest", "Open", LC_MESSAGES), "Ouvrir", "dcgettext");
    textdomain("gettexttest");
    same(bindtextdomain("gettexttest", NULL), "/usr/share/locale", "the default binding");
    bindtextdomain("gettexttest", "/nonexistent");
    same(gettext("Open"), "Open", "a binding to a missing directory");
    bindtextdomain("gettexttest", "/usr/share/locale");
    same(gettext("Open"), "Ouvrir", "a restored binding");
    same(bind_textdomain_codeset("gettexttest", "UTF-8"), "UTF-8", "bind_textdomain_codeset");
    setlocale(LC_ALL, "C");
}

static void *thread_main(void *arg)
{
    uselocale(arg);
    char *t = gettext("Open");
    int ok = strcmp(t, "Ouvrir") == 0;
    uselocale(LC_GLOBAL_LOCALE);
    return ok ? (void *)1 : NULL;
}

static void test_uselocale(void)
{
    locale_t fr = newlocale(LC_MESSAGES_MASK, "fr_FR.UTF-8", NULL);
    pthread_t t;
    void *ok = NULL;
    if (!fr || pthread_create(&t, NULL, thread_main, fr) != 0 || pthread_join(t, &ok) != 0 || !ok) {
        failures++;
        printf("gettexttest: FAIL uselocale selects the language of a thread\n");
    }
    same(gettext("Open"), "Open", "the main thread uses the C locale");
    freelocale(fr);
}

int main(void)
{
    same(textdomain(NULL), "messages", "the default domain");
    textdomain("gettexttest");
    test_french();
    test_russian();
    test_language();
    test_domains();
    test_uselocale();
    printf("gettexttest: %d failures\n", failures);
    return failures != 0;
}
