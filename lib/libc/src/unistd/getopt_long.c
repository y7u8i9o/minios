/* getopt_long of <getopt.h> in the manner of the GNU C library.  The
 * operands that appear between options are moved behind the options.
 * first and last mark the operands that the scan has passed and not yet
 * moved: argv[first] to argv[last - 1].  pos is the position of the next
 * option character inside a group of short options such as "-abc", or 0
 * at the start of an argument. */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum ordering { PERMUTE, REQUIRE_ORDER, RETURN_IN_ORDER };

static int first, last, pos, started;

static int is_operand(const char *s)
{
    return s[0] != '-' || s[1] == '\0';
}

static void reverse(char **argv, int from, int to)
{
    while (from < --to) {
        char *t = argv[from];
        argv[from] = argv[to];
        argv[to] = t;
        from++;
    }
}

/* exchange moves the options argv[last] to argv[optind - 1] in front of the
 * operands argv[first] to argv[last - 1].  The order inside each group is
 * unchanged. */
static void exchange(char **argv)
{
    reverse(argv, first, last);
    reverse(argv, last, optind);
    reverse(argv, first, optind);
    first += optind - last;
    last = optind;
}

static int long_option(int argc, char **argv, const struct option *longopts, int *longindex, int quiet)
{
    char *arg = argv[optind] + 2;
    char *eq = strchr(arg, '=');
    size_t len = eq ? (size_t)(eq - arg) : strlen(arg);
    int found = -1, ambiguous = 0;
    for (int i = 0; longopts[i].name; i++) {
        if (strncmp(longopts[i].name, arg, len) != 0)
            continue;
        if (strlen(longopts[i].name) == len) {
            found = i;
            ambiguous = 0;
            break;
        }
        if (found < 0) {
            found = i;
        } else if (longopts[i].has_arg != longopts[found].has_arg || longopts[i].flag != longopts[found].flag ||
                   longopts[i].val != longopts[found].val) {
            ambiguous = 1;
        }
    }
    optind++;
    optopt = 0;
    optarg = NULL;
    if (ambiguous) {
        if (!quiet && opterr)
            fprintf(stderr, "%s: option '--%.*s' is ambiguous\n", argv[0], (int)len, arg);
        return '?';
    }
    if (found < 0) {
        if (!quiet && opterr)
            fprintf(stderr, "%s: unrecognized option '--%s'\n", argv[0], arg);
        return '?';
    }
    const struct option *o = &longopts[found];
    if (eq) {
        if (o->has_arg == no_argument) {
            optopt = o->flag ? 0 : o->val;
            if (!quiet && opterr)
                fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n", argv[0], o->name);
            return '?';
        }
        optarg = eq + 1;
    } else if (o->has_arg == required_argument) {
        if (optind >= argc) {
            optopt = o->flag ? 0 : o->val;
            if (!quiet && opterr)
                fprintf(stderr, "%s: option '--%s' requires an argument\n", argv[0], o->name);
            return quiet ? ':' : '?';
        }
        optarg = argv[optind++];
    }
    if (longindex)
        *longindex = found;
    if (o->flag) {
        *o->flag = o->val;
        return 0;
    }
    return o->val;
}

int getopt_long(int argc, char *const argv_const[], const char *optstring, const struct option *longopts,
                int *longindex)
{
    /* The operands are moved inside argv, as the GNU C library does. */
    char **argv = (char **)argv_const;
    enum ordering ordering = PERMUTE;
    if (optstring[0] == '+') {
        ordering = REQUIRE_ORDER;
        optstring++;
    } else if (optstring[0] == '-') {
        ordering = RETURN_IN_ORDER;
        optstring++;
    } else if (getenv("POSIXLY_CORRECT")) {
        ordering = REQUIRE_ORDER;
    }
    int quiet = optstring[0] == ':';
    if (quiet)
        optstring++;

    if (optind == 0 || optreset || !started) {
        if (optind == 0)
            optind = 1;
        first = last = optind;
        pos = 0;
        optreset = 0;
        started = 1;
    }

    if (pos == 0) {
        /* The scan may have passed operands since the last call. */
        if (last > optind)
            last = optind;
        if (first > optind)
            first = optind;
        if (ordering == PERMUTE) {
            if (first != last && last != optind)
                exchange(argv);
            else if (last != optind)
                first = optind;
            while (optind < argc && is_operand(argv[optind]))
                optind++;
            last = optind;
        }
        if (optind != argc && strcmp(argv[optind], "--") == 0) {
            optind++;
            if (first != last && last != optind)
                exchange(argv);
            else if (first == last)
                first = optind;
            last = argc;
            optind = argc;
        }
        if (optind >= argc) {
            if (first != last)
                optind = first;
            return -1;
        }
        if (is_operand(argv[optind])) {
            if (ordering == REQUIRE_ORDER)
                return -1;
            optarg = argv[optind++];
            return 1;
        }
        if (longopts && argv[optind][1] == '-')
            return long_option(argc, argv, longopts, longindex, quiet);
        pos = 1;
    }

    char *group = argv[optind];
    int c = (unsigned char)group[pos++];
    const char *spec = c == ':' ? NULL : strchr(optstring, c);
    int end = group[pos] == '\0';
    if (!spec) {
        optopt = c;
        if (!quiet && opterr)
            fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
        if (end) {
            optind++;
            pos = 0;
        }
        return '?';
    }
    optarg = NULL;
    if (spec[1] != ':') {
        if (end) {
            optind++;
            pos = 0;
        }
        return c;
    }
    if (!end) {
        optarg = group + pos;
        optind++;
    } else if (spec[2] == ':') {
        optind++;
    } else if (optind + 1 < argc) {
        optarg = argv[optind + 1];
        optind += 2;
    } else {
        optopt = c;
        optind++;
        pos = 0;
        if (!quiet && opterr)
            fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], c);
        return quiet ? ':' : '?';
    }
    pos = 0;
    return c;
}
