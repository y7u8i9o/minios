#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <ctype.h>
#include <pwd.h>

#define MAX_ARGS 512
#define MAX_CMDS 32
#define MAX_JOBS 16
#define WORD_MAX 65536

enum node_kind {
    N_SIMPLE,
    N_PIPELINE,
    N_LIST,
    N_IF,
    N_FOR,
    N_WHILE,
    N_UNTIL,
    N_CASE,
    N_FUNCDEF,
    N_SUBSHELL,
    N_GROUP
};
struct word {
    char *text;
    unsigned char *quote;
    struct word *next;
};
enum redir_kind { R_IN, R_OUT, R_APPEND, R_DUPIN, R_DUPOUT, R_HEREDOC };
struct redir {
    int fd;
    enum redir_kind kind;
    struct word *target;
    char *body;
    int quoted, strip;
    struct redir *next;
};
struct node {
    enum node_kind kind;
    struct word *words;
    struct redir *redirs;
    struct node *a, *b, *c, *next;
    int op, bg;
    char *text;
};
enum token_kind {
    T_END,
    T_WORD,
    T_NL,
    T_IO,
    T_PIPE,
    T_AMP,
    T_SEMI,
    T_AND,
    T_OR,
    T_LP,
    T_RP,
    T_DSEMI,
    T_IN,
    T_OUT,
    T_APPEND,
    T_DUPIN,
    T_DUPOUT,
    T_HERE,
    T_HERETAB
};
struct token {
    enum token_kind kind;
    char *text;
    size_t start, end;
};
struct lexer {
    const char *source;
    size_t pos;
    int incomplete;
    struct token token;
};
struct reader {
    FILE *file;
    int interactive;
};
enum exec_flow { FLOW_NORMAL, FLOW_RETURN, FLOW_BREAK, FLOW_CONTINUE, FLOW_EXIT };
extern int interactive, last_status, script_argc, flow_count, loop_depth, function_depth;
/* The number of command substitutions that ran (capture). A command
 * without a command name returns the status of its last substitution. */
extern unsigned long substitutions;
/* set -e: a failing command outside a condition exits the shell. errexit_off
 * counts the enclosing contexts in which POSIX suspends the option. */
extern int opt_errexit, errexit_off;
/* set -f: pathname expansion is off. */
extern int opt_noglob;
/* set -o pipefail: the status of a pipeline is the status of the last
 * command that failed, or 0 when every command succeeded. */
extern int opt_pipefail;
/* exec without a command: the redirections of the command remain in
 * effect after it (execute_one). */
extern int keep_redirects;
/* trap.c */
int builtin_trap(int argc, char **argv);
void traps_run_pending(void);
/* The default dispositions for a program the shell executes. */
void child_signals(void);
extern char **script_argv;
extern pid_t last_background;
extern enum exec_flow flow;
struct job {
    int used, state;
    pid_t pgid, pids[MAX_CMDS];
    /* statuses[i] is the exit status of pids[i] after its exit, 0 before. */
    int statuses[MAX_CMDS];
    int npids;
    char text[64];
};
enum { JOB_RUNNING, JOB_STOPPED };
struct fields {
    char **v;
    int n;
};
struct parameters {
    int argc, owned;
    char **argv;
};
void parameters_replace(int argc, char **argv);
struct parameters parameters_push(int argc, char **argv);
void parameters_pop(struct parameters previous);
struct saved_fd {
    int fd, saved;
    pid_t writer;
    struct saved_fd *next;
};

char *sh_slice(const char *s, size_t n);
void *sh_alloc(size_t n);
void lex_next(struct lexer *lx);
void lex_destroy(struct lexer *lx);
/* status: 0 complete, 1 needs more input, 2 syntax error. */
struct node *parse(const char *source, int *status);
void node_free(struct node *n);
struct node *node_clone(const struct node *n);
char *word_literal(const char *text);
struct fields expand_words(struct word *w);
char *expand_one(const char *text, int pattern);
char *expand_here(const char *text);
void fields_free(struct fields *f);
long arith_eval(const char *text, int *error);
const char *var_get(const char *name);
const char *var_lookup(const char *name, char *tmp, size_t size);
int var_set(const char *name, const char *value, int exported);
int var_local(const char *name, const char *value);
void var_unset(const char *name);
void vars_init(void);
void vars_print(void);
void scope_push(void);
void scope_pop(void);
int valid_name(const char *name);
int assignment(const char *text);
const char *alias_get(const char *name);
int alias_set(const char *name, const char *value);
void alias_unset(const char *name);
void alias_print(void);
struct node *function_get(const char *name);
void function_set(const char *name, const struct node *body);
int exec_node(struct node *n);
int run_line(const char *line);
int run_reader(struct reader *reader);
int run_file(const char *path);
char *capture(const char *cmd);
int redirect_apply(struct redir *r, struct saved_fd **saved);
void redirect_restore(struct saved_fd *saved);
/* Drop the saved descriptors: the redirections remain in effect. */
void redirect_discard(struct saved_fd *saved);
int builtin(int argc, char **argv);
int is_builtin(const char *name);
int command_run(int argc, char **argv, int allow_functions);
int builtin_test(int argc, char **argv);
int builtin_ulimit(char **argv);
void jobs_reap(int block);
int builtin_jobs(void);
int builtin_bg(const char *arg);
int builtin_fg(const char *arg);
/* statuses may be NULL for a job whose processes are all running. */
int job_add(pid_t pgid, pid_t *pids, const int *statuses, int n, const char *text, int state, int announce);
int wait_foreground(struct job *j);
char *prompt_render(int secondary);
void prompt_startup(void);
void shell_history(int argc, char **argv);
