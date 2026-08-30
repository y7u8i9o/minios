/* cowsay: a cow says the arguments, or standard input. */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    char text[512] = "";
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (i > 1)
                strncat(text, " ", sizeof text - strlen(text) - 1);
            strncat(text, argv[i], sizeof text - strlen(text) - 1);
        }
    } else {
        size_t n = fread(text, 1, sizeof text - 1, stdin);
        text[n] = '\0';
        for (size_t i = 0; i < n; i++)
            if (text[i] == '\n')
                text[i] = ' ';
        while (n && text[n - 1] == ' ')
            text[--n] = '\0';
    }
    if (!text[0])
        strcpy(text, "Moo.");
    /* Wrap at 40 columns. */
    char lines[16][41];
    int nlines = 0, width = 0;
    const char *p = text;
    while (*p && nlines < 16) {
        int len = (int)strlen(p);
        if (len > 40) {
            len = 40;
            while (len > 0 && p[len] != ' ')
                len--;
            if (len == 0)
                len = 40;
        }
        snprintf(lines[nlines], sizeof lines[nlines], "%.*s", len, p);
        if (len > width)
            width = len;
        nlines++;
        p += len;
        while (*p == ' ')
            p++;
    }
    printf(" ");
    for (int i = 0; i < width + 2; i++)
        putchar('_');
    printf("\n");
    for (int i = 0; i < nlines; i++) {
        char l = nlines == 1 ? '<' : i == 0 ? '/' : i == nlines - 1 ? '\\' : '|';
        char r = nlines == 1 ? '>' : i == 0 ? '\\' : i == nlines - 1 ? '/' : '|';
        printf("%c %-*s %c\n", l, width, lines[i], r);
    }
    printf(" ");
    for (int i = 0; i < width + 2; i++)
        putchar('-');
    printf("\n"
           "        \\   ^__^\n"
           "         \\  (oo)\\_______\n"
           "            (__)\\       )\\/\\\n"
           "                ||----w |\n"
           "                ||     ||\n");
    return 0;
}
