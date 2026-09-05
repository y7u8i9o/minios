#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <regex.h>
#include <term.h>

static int icase, number, invert, count_only, names_only, recursive, word, fixed, extended, color;
static int name_policy = -1, multiple, found, errors;
static const char *pattern;
static regex_t regex;

static int word_char(int c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static int match(const char *line, size_t offset, regmatch_t *result)
{
    size_t length = strlen(line), pattern_length = strlen(pattern);
    while (offset <= length) {
        if (fixed) {
            int hit = 0;
            for (size_t at = offset; at + pattern_length <= length; at++) {
                size_t i = 0;
                while (i < pattern_length) {
                    int a = (unsigned char)line[at + i], b = (unsigned char)pattern[i];
                    if (icase) {
                        a = tolower(a);
                        b = tolower(b);
                    }
                    if (a != b)
                        break;
                    i++;
                }
                if (i == pattern_length) {
                    result->rm_so = (regoff_t)at;
                    result->rm_eo = (regoff_t)(at + i);
                    hit = 1;
                    break;
                }
            }
            if (!hit)
                return 0;
        } else {
            result->rm_so = (regoff_t)offset;
            result->rm_eo = (regoff_t)length;
            if (regexec(&regex, line, 1, result, REG_STARTEND | (offset ? REG_NOTBOL : 0)))
                return 0;
        }
        if (!word ||
            ((!result->rm_so || !word_char(line[result->rm_so - 1])) && !word_char(line[result->rm_eo])))
            return 1;
        offset = (size_t)result->rm_so + 1;
    }
    return 0;
}

static void search(FILE *file, const char *name, int show_name)
{
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    unsigned long lineno = 0, hits = 0;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        lineno++;
        if (length && line[length - 1] == '\n')
            line[--length] = 0;
        regmatch_t result;
        int matches = match(line, 0, &result);
        if (matches == invert)
            continue;
        found = 1;
        hits++;
        if (names_only) {
            puts(name);
            break;
        }
        if (count_only)
            continue;
        if (show_name)
            printf("%s:", name);
        if (number)
            printf("%lu:", lineno);
        if (color && matches && !invert) {
            size_t offset = 0;
            do {
                fwrite(line + offset, 1, (size_t)result.rm_so - offset, stdout);
                fputs("\033[31m", stdout);
                fwrite(line + result.rm_so, 1, (size_t)(result.rm_eo - result.rm_so), stdout);
                fputs("\033[0m", stdout);
                offset = (size_t)result.rm_eo;
                if (result.rm_so == result.rm_eo) {
                    if (offset == (size_t)length)
                        break;
                    putchar(line[offset++]);
                }
            } while (match(line, offset, &result));
            fputs(line + offset, stdout);
            putchar('\n');
        } else {
            puts(line);
        }
    }
    if (ferror(file))
        errors = 1;
    if (count_only && !names_only) {
        if (show_name)
            printf("%s:", name);
        printf("%lu\n", hits);
    }
    free(line);
}

static void search_path(const char *path, unsigned depth)
{
    struct stat st;
    if (stat(path, &st) < 0)
        goto error;
    if (S_ISDIR(st.st_mode) && recursive) {
        if (depth >= 128) {
            errno = ENAMETOOLONG;
            goto error;
        }
        DIR *dir = opendir(path);
        if (!dir)
            goto error;
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            size_t size = strlen(path) + strlen(entry->d_name) + 2;
            char *next = malloc(size);
            if (!next) {
                errors = 1;
                break;
            }
            snprintf(next, size, "%s/%s", path, entry->d_name);
            search_path(next, depth + 1);
            free(next);
        }
        closedir(dir);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        goto error;
    }
    if (depth && !S_ISREG(st.st_mode))
        return;
    FILE *file = fopen(path, "r");
    if (!file)
        goto error;
    search(file, path, name_policy < 0 ? multiple || recursive : name_policy);
    fclose(file);
    return;
error:
    fprintf(stderr, "grep: %s: %s\n", path, strerror(errno));
    errors = 1;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for (const char *p = argv[i] + 1; *p; p++) {
            switch (*p) {
            case 'E':
                extended = 1;
                fixed = 0;
                break;
            case 'F':
                fixed = 1;
                break;
            case 'i':
                icase = 1;
                break;
            case 'n':
                number = 1;
                break;
            case 'v':
                invert = 1;
                break;
            case 'c':
                count_only = 1;
                break;
            case 'l':
                names_only = 1;
                break;
            case 'r':
                recursive = 1;
                break;
            case 'h':
                name_policy = 0;
                break;
            case 'H':
                name_policy = 1;
                break;
            case 'w':
                word = 1;
                break;
            default:
                fprintf(stderr, "grep: unknown option -%c\n", *p);
                return 2;
            }
        }
    }
    if (i == argc) {
        fprintf(stderr, "usage: grep [-EFinvclrhHw] pattern [file...]\n");
        return 2;
    }
    pattern = argv[i++];
    color = term_use_color(1);
    if (!fixed) {
        int error = regcomp(&regex, pattern, (extended ? REG_EXTENDED : 0) | (icase ? REG_ICASE : 0));
        if (error) {
            char text[128];
            regerror(error, &regex, text, sizeof text);
            fprintf(stderr, "grep: %s\n", text);
            return 2;
        }
    }
    multiple = argc - i > 1;
    if (i == argc && recursive)
        search_path(".", 0);
    else if (i == argc)
        search(stdin, "(standard input)", name_policy == 1);
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-"))
            search(stdin, "(standard input)", name_policy < 0 ? multiple : name_policy);
        else
            search_path(argv[i], 0);
    }
    if (!fixed)
        regfree(&regex);
    return errors ? 2 : found ? 0 : 1;
}
