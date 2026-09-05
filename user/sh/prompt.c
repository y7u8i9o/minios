#include "sh.h"
#include <time.h>
#include <sys/stat.h>

char *prompt_render(int secondary)
{
    const char *format = var_get(secondary ? "PS2" : "PS1");
    if (!format)
        format = secondary ? "> " : "\\w \\$ ";
    char cwd[1024], clock[32];
    if (!getcwd(cwd, sizeof cwd))
        strcpy(cwd, "?");
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm || !strftime(clock, sizeof clock, "%H:%M:%S", tm))
        strcpy(clock, "00:00:00");
    size_t capacity = strlen(format) * 1024 + 1;
    char *out = sh_alloc(capacity);
    size_t used = 0;
    for (const char *p = format; *p; p++) {
        if (*p != '\\' || !p[1]) {
            out[used++] = *p;
            continue;
        }
        const char *value = NULL;
        char literal[2] = {0};
        switch (*++p) {
        case 'u': value = var_get("USER"); if (!value) value = "user"; break;
        case 'h': value = "minios"; break;
        case 'w': value = cwd; break;
        case 'W': value = strrchr(cwd, '/'); value = value && value[1] ? value + 1 : cwd; break;
        case '$': value = "$"; break;
        case 't': value = clock; break;
        case 'n': value = "\n"; break;
        case 'e': value = "\033"; break;
        case '[': case ']': value = ""; break;
        case '\\': value = "\\"; break;
        default: literal[0] = *p; value = literal; break;
        }
        size_t n = strlen(value);
        memcpy(out + used, value, n);
        used += n;
    }
    out[used] = 0;
    return out;
}

void prompt_startup(void)
{
    int status = last_status;
    struct stat st;
    if (stat("/etc/profile", &st) == 0)
        run_file("/etc/profile");
    const char *home = var_get("HOME");
    if (home && flow == FLOW_NORMAL) {
        size_t size = strlen(home) + 8;
        char *path = sh_alloc(size);
        snprintf(path, size, "%s/.shrc", home);
        if (stat(path, &st) == 0)
            run_file(path);
        free(path);
    }
    last_status = status;
}
