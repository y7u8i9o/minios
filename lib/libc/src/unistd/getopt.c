#include <unistd.h>
#include <stdio.h>
#include <string.h>

char *optarg;
int optind = 1;
int opterr = 1;
int optopt;
int optreset;

/* POSIX getopt. Options are collected from argv[optind] onwards until an
 * argument that does not start with '-', the argument "--" (skipped) or the
 * end of argv. A leading ':' in optstring silences the messages and makes a
 * missing argument return ':' instead of '?'. */
int getopt(int argc, char *const argv[], const char *optstring)
{
    static int pos;
    int quiet = optstring[0] == ':';

    if (optreset || optind == 0) {
        optind = 1;
        pos = 0;
        optreset = 0;
    }
    if (quiet)
        optstring++;

    if (pos == 0) {
        if (optind >= argc || argv[optind] == NULL || argv[optind][0] != '-' || argv[optind][1] == '\0')
            return -1;
        if (strcmp(argv[optind], "--") == 0) {
            optind++;
            return -1;
        }
        pos = 1;
    }

    int c = (unsigned char)argv[optind][pos++];
    const char *spec = c == ':' ? NULL : strchr(optstring, c);
    if (spec == NULL) {
        optopt = c;
        if (!quiet && opterr)
            fprintf(stderr, "%s: illegal option -- %c\n", argv[0], c);
        if (argv[optind][pos] == '\0') {
            optind++;
            pos = 0;
        }
        return '?';
    }
    if (spec[1] != ':') {
        if (argv[optind][pos] == '\0') {
            optind++;
            pos = 0;
        }
        return c;
    }
    if (argv[optind][pos] != '\0') {
        optarg = &argv[optind][pos];
    } else if (optind + 1 < argc) {
        optarg = argv[optind + 1];
        optind++;
    } else {
        optopt = c;
        optind++;
        pos = 0;
        if (quiet)
            return ':';
        if (opterr)
            fprintf(stderr, "%s: option requires an argument -- %c\n", argv[0], c);
        return '?';
    }
    optind++;
    pos = 0;
    return c;
}
