/* Highlighters for the editor: C and shell. state carries block
 * comment status between lines. */
#include <gui/widget.h>
#include <string.h>
#include <ctype.h>

static const char *const c_keywords[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum",
    "extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void",
    "volatile", "while", "bool", "true", "false", "NULL", NULL,
};

static const char *const sh_keywords[] = {
    "if", "then", "else", "elif", "fi", "for", "in", "do", "done", "while", "until", "case", "esac",
    "function", "return", "exit", "export", "local", "set", "cd", "echo", NULL,
};

static int is_keyword(const char *s, int n, const char *const *list)
{
    for (int i = 0; list[i]; i++)
        if ((int)strlen(list[i]) == n && memcmp(list[i], s, (size_t)n) == 0)
            return 1;
    return 0;
}

static int word_char(int c)
{
    return isalnum(c) || c == '_';
}

void highlight_c(const char *line, int len, unsigned char *classes, int *state, void *arg)
{
    int i = 0;
    memset(classes, HL_NORMAL, (size_t)len);
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (!*state && i < len && line[i] == '#') {
        memset(classes, HL_PREPROC, (size_t)len);
        return;
    }
    i = 0;
    while (i < len) {
        if (*state) {                                   /* inside a block comment */
            int end = -1;
            for (int j = i; j + 1 < len; j++)
                if (line[j] == '*' && line[j + 1] == '/') {
                    end = j + 2;
                    break;
                }
            int stop = end < 0 ? len : end;
            memset(classes + i, HL_COMMENT, (size_t)(stop - i));
            if (end < 0)
                return;
            *state = 0;
            i = end;
            continue;
        }
        char c = line[i];
        if (c == '/' && i + 1 < len && line[i + 1] == '/') {
            memset(classes + i, HL_COMMENT, (size_t)(len - i));
            return;
        }
        if (c == '/' && i + 1 < len && line[i + 1] == '*') {
            *state = 1;
            classes[i] = classes[i + 1] = HL_COMMENT;
            i += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            int j = i + 1;
            while (j < len && line[j] != c) {
                if (line[j] == '\\')
                    j++;
                j++;
            }
            if (j < len)
                j++;
            memset(classes + i, HL_STRING, (size_t)(j - i));
            i = j;
            continue;
        }
        if (isdigit((unsigned char)c) && (i == 0 || !word_char((unsigned char)line[i - 1]))) {
            int j = i;
            while (j < len && (isalnum((unsigned char)line[j]) || line[j] == '.'))
                j++;
            memset(classes + i, HL_NUMBER, (size_t)(j - i));
            i = j;
            continue;
        }
        if (word_char((unsigned char)c)) {
            int j = i;
            while (j < len && word_char((unsigned char)line[j]))
                j++;
            if (is_keyword(line + i, j - i, c_keywords))
                memset(classes + i, HL_KEYWORD, (size_t)(j - i));
            i = j;
            continue;
        }
        i++;
    }
}

void highlight_sh(const char *line, int len, unsigned char *classes, int *state, void *arg)
{
    int i = 0;
    memset(classes, HL_NORMAL, (size_t)len);
    while (i < len) {
        char c = line[i];
        if (c == '#') {
            memset(classes + i, HL_COMMENT, (size_t)(len - i));
            return;
        }
        if (c == '"' || c == '\'') {
            int j = i + 1;
            while (j < len && line[j] != c)
                j++;
            if (j < len)
                j++;
            memset(classes + i, HL_STRING, (size_t)(j - i));
            i = j;
            continue;
        }
        if (c == '$') {
            int j = i + 1;
            if (j < len && line[j] == '{') {
                while (j < len && line[j] != '}')
                    j++;
                if (j < len)
                    j++;
            } else {
                while (j < len && word_char((unsigned char)line[j]))
                    j++;
            }
            memset(classes + i, HL_PREPROC, (size_t)(j - i));
            i = j;
            continue;
        }
        if (isdigit((unsigned char)c) && (i == 0 || !word_char((unsigned char)line[i - 1]))) {
            int j = i;
            while (j < len && isdigit((unsigned char)line[j]))
                j++;
            memset(classes + i, HL_NUMBER, (size_t)(j - i));
            i = j;
            continue;
        }
        if (word_char((unsigned char)c)) {
            int j = i;
            while (j < len && word_char((unsigned char)line[j]))
                j++;
            if (is_keyword(line + i, j - i, sh_keywords))
                memset(classes + i, HL_KEYWORD, (size_t)(j - i));
            i = j;
            continue;
        }
        i++;
    }
}
