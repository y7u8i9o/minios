/* ld: link with the linker of tcc under the conventional name.
 *
 *     ld [options] file...
 *
 * The objects, archives and libraries named are linked by /bin/tcc with
 * -nostdlib, so nothing is added that was not named: the C runtime objects
 * and the libraries have to be given, as with any ld. The options of ld
 * that tcc implements are translated; the ones that name what tcc does
 * anyway (-dynamic-linker /lib/ld.so, -Ttext-segment=0x400000, the hash
 * style, -z options, --as-needed, groups) are accepted and dropped; any
 * other option is an error. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_ARGS 512

static char *args[MAX_ARGS];
static int nargs;

static void add(char *arg)
{
    if (nargs == MAX_ARGS - 1) {
        fprintf(stderr, "ld: too many arguments\n");
        exit(1);
    }
    args[nargs++] = arg;
}

static char *join(const char *a, const char *b)
{
    char *s = malloc(strlen(a) + strlen(b) + 1);
    if (s == NULL) {
        fprintf(stderr, "ld: out of memory\n");
        exit(1);
    }
    strcpy(s, a);
    strcat(s, b);
    return s;
}

/* An option that takes its value either attached or as the next word. */
static char *value_of(int argc, char **argv, int *i, size_t optlen)
{
    if (argv[*i][optlen] != '\0')
        return argv[*i] + optlen + (argv[*i][optlen] == '=');
    if (*i + 1 >= argc) {
        fprintf(stderr, "ld: %s needs a value\n", argv[*i]);
        exit(1);
    }
    return argv[++*i];
}

static int starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int main(int argc, char **argv)
{
    add("tcc");
    add("-nostdlib");
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '-') {
            add(a);
        } else if (strcmp(a, "-o") == 0 || starts(a, "-o=")) {
            add("-o");
            add(value_of(argc, argv, &i, 2));
        } else if (starts(a, "-L") || starts(a, "-l")) {
            if (a[2] == '\0') {
                char *v = value_of(argc, argv, &i, 2);
                add(join(a, v));
            } else {
                add(a);
            }
        } else if (strcmp(a, "-shared") == 0 || strcmp(a, "-static") == 0 || strcmp(a, "-r") == 0 ||
                   strcmp(a, "-rdynamic") == 0 || strcmp(a, "--export-dynamic") == 0 || strcmp(a, "-E") == 0) {
            add(strcmp(a, "--export-dynamic") == 0 || strcmp(a, "-E") == 0 ? "-rdynamic" : a);
        } else if (strcmp(a, "-soname") == 0 || strcmp(a, "-h") == 0 || starts(a, "-soname=") || starts(a, "--soname=")) {
            char *v = value_of(argc, argv, &i, a[1] == '-' ? 8 : (a[1] == 'h' ? 2 : 7));
            add("-soname");
            add(v);
        } else if (strcmp(a, "-Bsymbolic") == 0) {
            add("-Wl,-Bsymbolic");
        } else if (starts(a, "-rpath")) {
            add(join("-Wl,-rpath=", value_of(argc, argv, &i, 6)));
        } else if (strcmp(a, "-Map") == 0 || starts(a, "-Map=")) {
            add(join("-Wl,-Map=", value_of(argc, argv, &i, 4)));
        } else if (strcmp(a, "-e") == 0 || strcmp(a, "--entry") == 0 || starts(a, "--entry=") || starts(a, "-e=")) {
            char *v = value_of(argc, argv, &i, a[1] == '-' ? 7 : 2);
            if (strcmp(v, "_start") != 0) {
                fprintf(stderr, "ld: the entry point is _start; %s is not supported\n", v);
                return 1;
            }
        } else if (strcmp(a, "-dynamic-linker") == 0 || strcmp(a, "--dynamic-linker") == 0 ||
                   starts(a, "-dynamic-linker=") || starts(a, "--dynamic-linker=")) {
            char *v = value_of(argc, argv, &i, a[1] == '-' ? 16 : 15);
            if (strcmp(v, "/lib/ld.so") != 0) {
                fprintf(stderr, "ld: the interpreter is /lib/ld.so; %s is not supported\n", v);
                return 1;
            }
        } else if (starts(a, "-Ttext-segment") || starts(a, "-Ttext")) {
            char *v = value_of(argc, argv, &i, starts(a, "-Ttext-segment") ? 14 : 6);
            if (strtoul(v, NULL, 0) != 0x400000) {
                fprintf(stderr, "ld: the text segment is at 0x400000; %s is not supported\n", v);
                return 1;
            }
        } else if (strcmp(a, "-z") == 0) {
            i++;                                /* max-page-size, now, relro and the others */
        } else if (starts(a, "-z") || starts(a, "--hash-style") || strcmp(a, "--as-needed") == 0 ||
                   strcmp(a, "--no-as-needed") == 0 || strcmp(a, "--start-group") == 0 ||
                   strcmp(a, "--end-group") == 0 || strcmp(a, "-(") == 0 || strcmp(a, "-)") == 0 ||
                   strcmp(a, "--no-dynamic-linker") == 0 || strcmp(a, "-nostdlib") == 0 ||
                   strcmp(a, "--eh-frame-hdr") == 0 || strcmp(a, "--build-id") == 0 ||
                   strcmp(a, "-m") == 0 || starts(a, "-m")) {
            if (strcmp(a, "-m") == 0)
                i++;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            printf("ld: the linker of tcc\n");
            add("-v");
        } else if (strcmp(a, "--help") == 0) {
            printf("usage: ld [-o file] [-L dir] [-l name] [-shared] [-static] [-r] [-soname name] file...\n");
            return 0;
        } else {
            fprintf(stderr, "ld: unsupported option %s\n", a);
            return 1;
        }
    }
    args[nargs] = NULL;
    execv("/bin/tcc", args);
    perror("ld: /bin/tcc");
    return 127;
}
