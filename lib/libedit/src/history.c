#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void edit_history_limit(struct edit *editor, size_t limit)
{
    editor->history_limit = limit;
    while (editor->history_count > limit) {
        free(editor->history[0]);
        editor->history_count--;
        memmove(editor->history, editor->history + 1, editor->history_count * sizeof *editor->history);
    }
}

int edit_history_add(struct edit *editor, const char *line)
{
    if (!editor->history_limit || !*line)
        return 0;
    if (editor->history_count && !strcmp(editor->history[editor->history_count - 1], line))
        return 0;
    char *copy = strdup(line);
    if (!copy)
        return -1;
    if (editor->history_count == editor->history_limit)
        edit_history_limit(editor, editor->history_limit - 1), editor->history_limit++;
    char **next = realloc(editor->history, (editor->history_count + 1) * sizeof *next);
    if (!next) {
        free(copy);
        return -1;
    }
    editor->history = next;
    editor->history[editor->history_count++] = copy;
    return 0;
}

size_t edit_history_count(const struct edit *editor)
{
    return editor->history_count;
}

const char *edit_history_get(const struct edit *editor, size_t index)
{
    return index < editor->history_count ? editor->history[index] : NULL;
}

int edit_history_load(struct edit *editor, const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return -1;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        if (length && line[length - 1] == '\n')
            line[length - 1] = 0;
        if (edit_history_add(editor, line) < 0)
            break;
    }
    free(line);
    int result = ferror(file) ? -1 : 0;
    fclose(file);
    return result;
}

int edit_history_save(struct edit *editor, const char *path)
{
    FILE *file = fopen(path, "w");
    if (!file)
        return -1;
    for (size_t i = 0; i < editor->history_count; i++)
        fprintf(file, "%s\n", editor->history[i]);
    int result = ferror(file) ? -1 : 0;
    if (fclose(file))
        result = -1;
    return result;
}
