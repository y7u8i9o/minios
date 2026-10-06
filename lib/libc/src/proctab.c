/* The process table of /dev/proc (minios/proctab.h). */
#include <minios/proctab.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The whole table as a string. The caller frees it. */
static char *read_table(void)
{
    int fd = open("/dev/proc", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    size_t cap = 8192, used = 0;
    char *text = malloc(cap);
    while (text) {
        if (used + 1 >= cap) {
            char *grown = realloc(text, cap * 2);
            if (!grown) {
                free(text);
                text = NULL;
                break;
            }
            text = grown;
            cap *= 2;
        }
        ssize_t n = read(fd, text + used, cap - 1 - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        used += (size_t)n;
    }
    int err = errno;
    close(fd);
    if (text)
        text[used] = '\0';
    errno = err;
    return text;
}

/* One data row. Returns 0, or -1 for the header and malformed lines. */
static int parse_row(const char *line, struct proc_entry *e)
{
    char state[sizeof e->state], name[sizeof e->name];
    int pid, ppid, pgid;
    unsigned long ticks, rss;
    unsigned uid;
    int used = 0;
    if (sscanf(line, "%d %d %d %11s %lu %lu %u %n", &pid, &ppid, &pgid, state, &ticks, &rss, &uid, &used) != 7)
        return -1;
    /* The name is the rest of the line. */
    size_t n = strcspn(line + used, "\n");
    if (n >= sizeof name)
        n = sizeof name - 1;
    memcpy(name, line + used, n);
    name[n] = '\0';
    *e = (struct proc_entry){ .pid = pid, .ppid = ppid, .pgid = pgid, .ticks = ticks, .rss_kib = rss, .uid = uid };
    strlcpy(e->state, state, sizeof e->state);
    strlcpy(e->name, name, sizeof e->name);
    return 0;
}

int proc_table_read(struct proc_entry *rows, int max)
{
    char *text = read_table();
    if (!text)
        return -errno;
    int n = 0;
    for (char *line = text; *line && n < max;) {
        if (parse_row(line, &rows[n]) == 0)
            n++;
        char *next = strchr(line, '\n');
        if (!next)
            break;
        line = next + 1;
    }
    free(text);
    return n;
}

int proc_table_find(pid_t pid, struct proc_entry *out)
{
    char *text = read_table();
    if (!text)
        return -errno;
    int r = -ESRCH;
    for (char *line = text; *line;) {
        if (parse_row(line, out) == 0 && out->pid == pid) {
            r = 0;
            break;
        }
        char *next = strchr(line, '\n');
        if (!next)
            break;
        line = next + 1;
    }
    free(text);
    return r;
}
