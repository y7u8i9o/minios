# Shell

`user/sh/` implements `/bin/sh`. It parses complete commands into an owned
syntax tree, then expands words when a node executes. Parsing does not run
substitutions or bind loop variables. `sh file args...` runs a script;
`sh -c 'commands'` evaluates a string. Interactive input uses [libedit](libedit.md).

## Reader and grammar

`main.c` owns the reader, startup and history. `lexer.c` retains raw word
text, including quotes, escapes and nested substitutions. Operators and
adjacent descriptor numbers are tokens; the parser recognizes reserved
words only in command positions. `parser.c` follows this precedence:

    complete command -> list -> and/or -> pipeline -> command
    command -> simple | if | for | while | until | case
             | function definition | subshell | brace group

Lists accept newlines, semicolons and background `&`; and/or lists use
`&&` and `||`. A parse result distinguishes a complete tree, incomplete
input and a syntax error. Incomplete input requests another line using
`PS2`. An unclosed quote or substitution, an open compound command, and a
line that ends in a backslash outside single quotes are incomplete
input. A command continued with a backslash and a newline therefore runs
once, with the words of both lines. Here-document bodies are collected after the command's newline,
in redirection order, before the tree is executed.

`sh.h` defines `struct node`, words and redirections. Nodes own their
children and linked siblings; `node_free` releases the whole tree.
Functions retain a cloned body, so freeing the defining command does not
invalidate a later invocation. `node.text` preserves job labels.

## Expansion and state

`expand.c` performs tilde expansion, parameter/command/arithmetic
substitution, IFS field splitting, pathname expansion and quote removal.
The intermediate byte flags preserve quoted characters and separate
quoted `$@` arguments, including empty arguments. Quoted glob characters
remain literal. Command substitution captures a child's pipe output and
removes trailing newlines. Parameter operators include defaults,
assignment, alternate values, errors, length and prefix/suffix removal.
`arith.c` implements integer precedence, variable lookup, assignment,
short-circuit boolean operators and the conditional operator. The
expression of `$((...))` undergoes parameter expansion, command
substitution and quote removal before it is evaluated, as POSIX requires,
so `$((${n:-7} % 8))` works (2026-10-05).

`vars.c` imports the environment into its own table. Exported updates
also update `environ`; ordinary shell variables remain private. Function
calls push a scope, and `local` shadows an outer binding until scope pop.
Positional parameters are separately owned copies: `shift` and `set --`
must not mutate an expanded command's argument array. Calls save and
restore the caller's parameters. `$!` records the last background group.
Aliases are reparsed at execution time with a recursion limit; this is
not POSIX's read-time alias expansion.

## Execution and redirections

`exec.c` evaluates trees and propagates `return`, `break`, `continue` and
`exit` through an explicit flow state. Groups execute in the current
shell; subshells and pipeline stages fork. Builtins and functions can
therefore change the current shell when not placed in a pipeline.
`builtins.c`, `builtin_test.c` and `ulimit.c` provide the command table,
tests and resource limits. `help` preserves its historical prefix.
`set -e` and `set +e` switch the errexit option: a command that fails
outside a condition of `if`, `while` or `until`, outside the left side
of `&&` and `||`, and not negated with `!`, exits the shell with its
status (`errexit_off` in `exec.c` counts those contexts).
`set -f` and `set +f` switch off and on pathname expansion
(`opt_noglob`). `set -o NAME` and `set +o NAME` switch an option by its
name: `errexit`, `noglob` or `pipefail`. `pipefail` exists only by name.
With `pipefail` the status of a pipeline is the status of the rightmost
command that failed, or 0 when every command succeeded. A command that
a signal ended counts as failed with 128 plus the signal number, SIGPIPE
included, as in bash. Each job records the exit status of every process
in `statuses` for that purpose (`jobs.c`).

A command without a command name, such as `x=$(rmdir dir)`, returns the
status of its last command substitution, and 0 without a substitution
(POSIX 2.9.1). `capture` counts the substitutions in `substitutions`, and
`simple` compares the count before and after the expansion. Before V5 of
the 0.6.0 release such a command always returned 0. `set -o` without a name prints
each option with `on` or `off`. `set +o` prints the `set` commands that
restore the current states. Any other option letter or option name is
refused with status 2. `set` alone prints the variables; `set --` or a first non option word replaces the positional
parameters. pdpmake in POSIX mode prefixes every command with `set -e;`.

`redirect.c` handles `<`, `>`, `>>`, descriptor-qualified forms, `<&`,
`>&`, descriptor closure and `<<`/`<<-`. Saved descriptors are
close-on-exec and are restored even after a failed application. A quoted
here-document delimiter suppresses expansion; `<<-` strips leading tabs.
A writer child feeds the here-document pipe so a document larger than
the pipe capacity cannot block the shell before its reader starts.

`exec` without a command sets `keep_redirects`, and `execute_one` then
drops the saved descriptors (`redirect_discard`) instead of restoring
them, so `exec 6>&1 >/dev/null` changes the descriptors of the shell
itself. `exec` with a command replaces the shell with it and exits with
127 or 126 when it cannot be executed. `trap` (`trap.c`) records an action
for `EXIT`, a signal name with or without `SIG`, or a number. A caught
signal only marks its trap, and `exec_node` runs the marked actions after
the current command with `$?` retained. An empty action ignores the
signal, and `-` restores the disposition the shell had before. The `EXIT`
action runs at the exit of the shell process that set it, through
`atexit`, and not in the subshells forked from it. `trap` alone prints the
traps in a form that can be read again. The three builtins were added for
pfetch (`pfetch.md`), and the case `sh_builtins` runs
`/etc/tests/shbuiltins.sh`, which uses each of them.

## Job control

Every pipeline gets its own process group. Foreground pipelines own the
terminal through `tcsetpgrp`; control Z stops them and records a job.
Interactive background readers stop with `SIGTTIN`. `jobs.c` reports
Running/Stopped state, `bg [%n]` continues without taking the terminal,
and `fg [%n]` gives it the terminal and waits. Non-interactive background
commands without input redirection use `/dev/null`. Existing start,
stop and Done messages are retained. Children restore default signal
handling; the interactive shell ignores `SIGINT` and `SIGPIPE`.

## Interactive defaults

Init sets `PATH=/usr/bin TERM=minios` from the `env` line of
`/etc/init.conf`, and since U3 of the multiuser plan (`users.md`) `login`
adds `HOME`, `USER`, `LOGNAME` and `SHELL` of the account and
`/usr/local/bin` to `PATH`. `/usr/bin` contains every program since P1 of
`docs/plan/packaging.md`, and `/bin` is a link to it. The prompt escape `\$` prints `#` for
effective uid 0, `\u` falls back to the account of the effective uid
without `USER`, and `~name` expands to the home of the account `name`.
Interactive shells source `/etc/profile`, then `$HOME/.shrc`. The profile
sets `PAGER=less`, `ll` and `la` aliases, and a coloured prompt when colour
is enabled. Without these environment defaults the prompt remains `/ $ `.
`prompt.c` expands `PS1`/`PS2` escapes for user, hostname, directory, time,
newline, escape and nonprinting delimiters. History defaults to
`$HOME/.sh_history` and 500 entries, configurable at startup through
`HISTFILE` and `HISTSIZE`; it is saved on normal shell exit.

## Tests

`make check-sh` runs host lexer, tree, quoting, glob, arithmetic and
execution tests. It can also run with `HOSTCC='cc -fsanitize=address,undefined'`.
`script2` executes `/etc/tests/sh_ctrl.sh` and rejects `FAIL` lines.
`shell`, `shell2`, `script`, `jobcontrol`, `ctrlc` and `pipes` preserve
the original behavior; `lineedit` exercises editing and completion.
`lineedit_screen` checks rendered cells after repeated invalid commands,
including a clean coloured prompt, to catch output-order regressions.
