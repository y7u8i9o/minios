/* Launch the Lua application; the synthesizer and its DSP live in Lua. */
#include <stdio.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    char *args[argc + 2];
    args[0] = "lua";
    args[1] = "/usr/share/apps/luasynth/main.lua";
    for (int i = 1; i < argc; i++) args[i + 1] = argv[i];
    args[argc + 1] = NULL;
    execv("/bin/lua", args);
    perror("luasynth: lua");
    return 127;
}
