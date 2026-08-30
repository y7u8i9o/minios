#include <lib/cmdline.h>
#include <lib/string.h>

static const char *kernel_cmdline = "";

void cmdline_init(const char *cmdline)
{
    kernel_cmdline = cmdline ? cmdline : "";
}

const char *cmdline_get(void)
{
    return kernel_cmdline;
}

bool cmdline_lookup(const char *key, char *buf, size_t size)
{
    size_t klen = strlen(key);
    const char *p = kernel_cmdline;

    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        const char *word = p;
        while (*p && *p != ' ')
            p++;
        size_t wlen = (size_t)(p - word);

        if (wlen >= klen && strncmp(word, key, klen) == 0 &&
            (wlen == klen || word[klen] == '=')) {
            const char *val = word + klen;
            size_t vlen = 0;
            if (wlen > klen) {
                val++;
                vlen = wlen - klen - 1;
            }
            if (size) {
                size_t n = vlen < size - 1 ? vlen : size - 1;
                memcpy(buf, val, n);
                buf[n] = '\0';
            }
            return true;
        }
    }
    if (size)
        buf[0] = '\0';
    return false;
}
