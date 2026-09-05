#include "internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

int edit_completion_add(struct edit_completions *matches, const char *value)
{
    for (size_t i = 0; i < matches->count; i++)
        if (!strcmp(matches->items[i], value))
            return 0;
    char *copy = strdup(value);
    if (!copy)
        return -1;
    char **items = realloc(matches->items, (matches->count + 1) * sizeof *items);
    if (!items) {
        free(copy);
        return -1;
    }
    matches->items = items;
    matches->items[matches->count++] = copy;
    return 0;
}

static void directory_matches(const char *directory, const char *prefix, const char *lead, int executable,
                              struct edit_completions *matches)
{
    DIR *dir = opendir(*directory ? directory : ".");
    if (!dir)
        return;
    size_t prefix_length = strlen(prefix);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if ((!*prefix && entry->d_name[0] == '.') || strncmp(entry->d_name, prefix, prefix_length))
            continue;
        size_t size = strlen(directory) + strlen(entry->d_name) + 3;
        char *path = malloc(size);
        if (!path)
            break;
        snprintf(path, size, "%s/%s", *directory ? directory : ".", entry->d_name);
        struct stat st;
        int found = stat(path, &st) == 0;
        free(path);
        if (!found || (executable && (!S_ISREG(st.st_mode) || !(st.st_mode & 0111))))
            continue;
        size = strlen(lead) + strlen(entry->d_name) + 2;
        char *value = malloc(size);
        if (!value)
            break;
        snprintf(value, size, "%s%s%s", lead, entry->d_name, S_ISDIR(st.st_mode) ? "/" : "");
        edit_completion_add(matches, value);
        free(value);
    }
    closedir(dir);
}

void edit_complete_files(const char *line, size_t cursor, struct edit_completions *matches, void *arg)
{
    size_t start = cursor;
    while (start && !strchr(" \t;|&<>", line[start - 1]))
        start--;
    matches->start = start;
    char *word = malloc(cursor - start + 1);
    if (!word)
        return;
    memcpy(word, line + start, cursor - start);
    word[cursor - start] = 0;
    char *slash = strrchr(word, '/');
    if (slash) {
        *slash = 0;
        size_t lead_length = (size_t)(slash - word) + 1;
        char *lead = malloc(lead_length + 1);
        if (lead) {
            memcpy(lead, line + start, lead_length);
            lead[lead_length] = 0;
            directory_matches(*word ? word : "/", slash + 1, lead, 0, matches);
            free(lead);
        }
    } else {
        directory_matches(".", word, "", 0, matches);
        size_t before = start;
        while (before && (line[before - 1] == ' ' || line[before - 1] == '\t'))
            before--;
        if (!before || strchr(";|&", line[before - 1])) {
            const char *path = getenv("PATH");
            if (!path)
                path = "/bin";
            while (*path) {
                const char *end = strchr(path, ':');
                size_t length = end ? (size_t)(end - path) : strlen(path);
                char *directory = malloc(length + 1);
                if (!directory)
                    break;
                memcpy(directory, path, length);
                directory[length] = 0;
                directory_matches(directory, word, "", 1, matches);
                free(directory);
                if (!end)
                    break;
                path = end + 1;
            }
        }
    }
    free(word);
}

void edit_complete(struct edit *editor, char *buf, size_t size)
{
    struct edit_completions matches = {.start = editor->cursor};
    editor->complete(buf, editor->cursor, &matches, editor->complete_arg);
    if (matches.count && matches.start <= editor->cursor) {
        size_t common = strlen(matches.items[0]);
        for (size_t i = 1; i < matches.count; i++) {
            size_t j = 0;
            while (j < common && matches.items[i][j] == matches.items[0][j])
                j++;
            common = j;
        }
        size_t replaced = editor->cursor - matches.start;
        int space = matches.count == 1 && common && matches.items[0][common - 1] != '/';
        if (common >= replaced && editor->length - replaced + common + space < size) {
            memmove(buf + matches.start + common + space, buf + editor->cursor,
                    editor->length - editor->cursor + 1);
            memcpy(buf + matches.start, matches.items[0], common);
            if (space)
                buf[matches.start + common] = ' ';
            editor->length = editor->length - replaced + common + space;
            editor->cursor = matches.start + common + space;
        }
    }
    for (size_t i = 0; i < matches.count; i++)
        free(matches.items[i]);
    free(matches.items);
}
