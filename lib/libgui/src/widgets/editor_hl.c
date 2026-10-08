/* Highlighters for the editor: C and shell, and a generic one driven by a
 * language description, with C, Lua and shell as the languages given.
 * state carries block comment status between lines. */
#include <gui/widget.h>
#include <string.h>
#include <ctype.h>

const uint32_t highlight_colors[HL_COUNT] = {
    [HL_KEYWORD] = 0x001040c0, [HL_STRING] = 0x00b03020, [HL_COMMENT] = 0x00308030,
    [HL_NUMBER] = 0x00901090,  [HL_PREPROC] = 0x00806020,
};

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

/* A highlighter driven by a language description. The state carries a
 * block comment across lines. */
static const char *const lua_keywords[] = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
    "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while", NULL,
};

const struct highlight_language highlight_language_c = { c_keywords, "//", "/*", "*/", "\"'", '#' };
const struct highlight_language highlight_language_lua = { lua_keywords, "--", "--[[", "]]", "\"'", 0 };
const struct highlight_language highlight_language_sh = { sh_keywords, "#", NULL, NULL, "\"'", '$' };

static int starts_with(const char *line, int len, int i, const char *s)
{
    int n = (int)strlen(s);
    return i + n <= len && memcmp(line + i, s, (size_t)n) == 0;
}

void highlight_lang(const char *line, int len, unsigned char *classes, int *state, void *arg)
{
    const struct highlight_language *lang = arg;
    int i = 0;
    memset(classes, HL_NORMAL, (size_t)len);
    if (lang->preproc == '#') {
        while (i < len && (line[i] == ' ' || line[i] == '\t'))
            i++;
        if (!*state && i < len && line[i] == '#') {
            memset(classes, HL_PREPROC, (size_t)len);
            return;
        }
        i = 0;
    }
    while (i < len) {
        if (*state) {
            int end = -1;
            for (int j = i; j < len; j++)
                if (starts_with(line, len, j, lang->block_end)) {
                    end = j + (int)strlen(lang->block_end);
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
        if (lang->block_start && starts_with(line, len, i, lang->block_start)) {
            *state = 1;
            int n = (int)strlen(lang->block_start);
            memset(classes + i, HL_COMMENT, (size_t)n);
            i += n;
            continue;
        }
        if (lang->line_comment && starts_with(line, len, i, lang->line_comment)) {
            memset(classes + i, HL_COMMENT, (size_t)(len - i));
            return;
        }
        if (lang->quotes && strchr(lang->quotes, c)) {
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
        if (lang->preproc == '$' && c == '$') {
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
            if (lang->keywords && is_keyword(line + i, j - i, lang->keywords))
                memset(classes + i, HL_KEYWORD, (size_t)(j - i));
            i = j;
            continue;
        }
        i++;
    }
}
