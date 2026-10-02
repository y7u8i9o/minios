#include "sh.h"
#include <assert.h>
#include <sys/stat.h>

static void tree_tests(void)
{
    int status;
    struct node *n = parse("if true; then echo 'a b' | cat; else false; fi", &status);
    assert(!status && n && n->kind == N_IF);
    assert(n->a->kind == N_SIMPLE && n->b->kind == N_PIPELINE);
    assert(!strcmp(n->b->a->words->next->text, "'a b'"));
    node_free(n);
    n = parse("for x in a b; do echo $x; done\nf() { :; }\n", &status);
    assert(!status && n->kind == N_FOR && n->next->kind == N_FUNCDEF);
    node_free(n);
    n = parse("echo \"unfinished", &status);
    assert(!n && status == 1);
    n = parse("if true; then\n", &status);
    assert(!n && status == 1);
    /* A line that ends in a backslash continues on the next one. */
    n = parse("echo a \\\n", &status);
    assert(!n && status == 1);
    n = parse("echo a \\\nb\n", &status);
    assert(!status && n && n->kind == N_SIMPLE && !strcmp(n->words->next->next->text, "b"));
    node_free(n);
    n = parse("echo 'a \\\n", &status);
    assert(!n && status == 1);
    n = parse("echo | ;", &status);
    assert(!n && status == 2);
    n = parse("read x <<'EOF'\nhello $x\nEOF\n", &status);
    assert(!status && n->redirs->quoted && !strcmp(n->redirs->body, "hello $x\n"));
    node_free(n);
    struct lexer lx = {.source = "2>>x && echo $(echo nested)\n"};
    enum token_kind tokens[] = {T_IO, T_APPEND, T_WORD, T_AND, T_WORD, T_WORD, T_NL, T_END};
    for (size_t i = 0; i < sizeof tokens / sizeof *tokens; i++) {
        lex_next(&lx);
        assert(lx.token.kind == tokens[i]);
    }
    lex_destroy(&lx);
}

static void expansion_tests(void)
{
    int error;
    assert(arith_eval("2 + 3 * 4", &error) == 14 && !error);
    assert(arith_eval("(2 + 3) * 4", &error) == 20 && !error);
    assert(arith_eval("0 && 1 / 0", &error) == 0 && !error);
    assert(arith_eval("1 ? 7 : 1 / 0", &error) == 7 && !error);
    arith_eval("1 / 0", &error);
    assert(error);
    var_set("WORDS", "one two", 0);
    int status;
    struct node *n = parse("$WORDS '$WORDS' \"$WORDS\" \\*", &status);
    assert(!status);
    struct fields f = expand_words(n->words);
    assert(f.n == 5 && !strcmp(f.v[0], "one") && !strcmp(f.v[2], "$WORDS") && !strcmp(f.v[3], "one two") &&
           !strcmp(f.v[4], "*"));
    fields_free(&f);
    node_free(n);
    char *value = expand_one("$((2 + 3 * 4))", 0);
    assert(!strcmp(value, "14"));
    free(value);
    value = expand_one("${WORDS% two}", 0);
    assert(!strcmp(value, "one"));
    free(value);

    char fixture[] = "/tmp/minios-sh-XXXXXX";
    assert(mkdtemp(fixture));
    char old[4096];
    assert(getcwd(old, sizeof old) && !chdir(fixture));
    FILE *file = fopen("b.txt", "w");
    assert(file);
    fclose(file);
    file = fopen("a.txt", "w");
    assert(file);
    fclose(file);
    file = fopen(".hidden", "w");
    assert(file);
    fclose(file);
    struct word word = {.text = "*.txt"};
    f = expand_words(&word);
    assert(f.n == 2 && !strcmp(f.v[0], "a.txt") && !strcmp(f.v[1], "b.txt"));
    fields_free(&f);
    unlink("a.txt");
    unlink("b.txt");
    unlink(".hidden");
    assert(!chdir(old));
    rmdir(fixture);
}

static void execution_tests(void)
{
    assert(run_line("args() { shift; test \"$1\" = two; }; args one two") == 0);
    assert(run_line("set -- 'a b' ''; test $# -eq 2; test \"$2\" = ''") == 0);
    struct word all = { .text = "\"$@\"" };
    struct fields arguments = expand_words(&all);
    assert(arguments.n == 2 && !strcmp(arguments.v[0], "a b") && !*arguments.v[1]);
    fields_free(&arguments);
    assert(run_line("shift; test $# -eq 1 && test \"$1\" = ''") == 0);
    assert(run_line("x=0; for i in 1 2 3; do x=$((x+i)); done; test $x -eq 6") == 0);
    assert(run_line("while test $x -gt 0; do x=$((x-1)); done; test $x -eq 0") == 0);
    assert(run_line("until test $x -eq 2; do x=$((x+1)); done; test $x -eq 2") == 0);
    assert(run_line("f() { local x=inner; test $x = inner; return 7; }; f") == 7);
    assert(!strcmp(var_get("x"), "2"));
    assert(run_line("case hello in nope) false;; h*) true;; esac") == 0);
    assert(run_line("read first rest <<EOF\nhello two words\nEOF\ntest \"$rest\" = 'two words'") == 0);
    assert(run_line("false && x=no; true || x=no; test $x = 2") == 0);
    char *out = capture("echo $(echo nested); echo tail");
    assert(!strcmp(out, "nested\ntail"));
    free(out);
    out = capture("echo hi | cat");
    assert(!strcmp(out, "hi"));
    free(out);
    assert(run_line("alias greet='echo hello'; test \"$(greet world)\" = 'hello world'") == 0);
}

int main(void)
{
    vars_init();
    char *args[] = {"sh", NULL};
    script_argc = 1;
    script_argv = args;
    tree_tests();
    expansion_tests();
    execution_tests();
    puts("check-sh: parser, expansion, arithmetic and execution passed");
    return 0;
}
