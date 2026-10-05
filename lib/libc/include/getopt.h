#pragma once
/* Long options in the manner of the GNU C library.  getopt_long shares
 * optarg, optind, opterr and optopt with getopt (<unistd.h>). */
#include <unistd.h>

struct option {
    const char *name;
    int has_arg;    /* no_argument, required_argument or optional_argument */
    int *flag;      /* NULL: getopt_long returns val; else *flag = val, result 0 */
    int val;
};

#define no_argument 0
#define required_argument 1
#define optional_argument 2

/* getopt_long parses the short options of optstring and the long options
 * of longopts, an array that ends with an entry of a NULL name.
 *
 * A long option is "--name", "--name=value" or "--name value".  A unique
 * prefix of a name selects the option, and an exact name takes precedence
 * over a prefix.  When longindex is not NULL, *longindex receives the index
 * of the option in longopts.
 *
 * By default, options and operands may appear in any order.  getopt_long
 * moves the operands behind the options, in their original order, and
 * optind receives the index of the first operand at the end.  "--" ends
 * the options.  A '+' at the start of optstring, or the environment
 * variable POSIXLY_CORRECT, ends the options at the first operand instead.
 * A '-' at the start of optstring returns each operand as the option 1
 * with the operand in optarg.  A ':' after these characters silences the
 * messages and returns ':' for a missing argument.
 *
 * The result is the option character or val, 0 when a flag was set, '?'
 * for an unknown or ambiguous option or a wrong argument, and -1 after the
 * last option.  Setting optind to 0 restarts the parsing. */
int getopt_long(int argc, char *const argv[], const char *optstring, const struct option *longopts,
                int *longindex);
