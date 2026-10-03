/* code: start the Code editor, a Lua program, with the arguments. The
 * launcher exists because the file type table names programs without
 * arguments. */
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char *args[argc + 3];
    args[0] = "lua";
    args[1] = "/usr/share/apps/code.lua";
    for (int i = 1; i < argc; i++)
        args[i + 1] = argv[i];
    args[argc + 1] = NULL;
    execv("/bin/lua", args);
    perror("code: /bin/lua");
    return 127;
}
