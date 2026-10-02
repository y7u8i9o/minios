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
short-circuit boolean operators and the conditional operator.

`vars.c` imports the environment into its own table. Exported updates
also update `environ`; ordinary shell variables stay private. Function
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
status (`errexit_off` in `exec.c` counts those contexts). Any other
option letter is refused with status 2. `set` alone prints the
variables; `set --` or a first non option word replaces the positional
parameters. pdpmake in POSIX mode prefixes every command with `set -e;`.

`redirect.c` handles `<`, `>`, `>>`, descriptor-qualified forms, `<&`,
`>&`, descriptor closure and `<<`/`<<-`. Saved descriptors are
close-on-exec and are restored even after a failed application. A quoted
here-document delimiter suppresses expansion; `<<-` strips leading tabs.
A writer child feeds the here-document pipe so a document larger than
the pipe capacity cannot block the shell before its reader starts.

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

Init sets `PATH=/bin HOME=/home USER=user SHELL=/bin/sh TERM=minios`
from the `env` line of `/etc/init.conf`.
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
