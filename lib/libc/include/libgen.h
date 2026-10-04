#pragma once

/* Both may modify the argument and return a pointer into it or into a
 * static buffer. */
char *basename(char *path);
char *dirname(char *path);
