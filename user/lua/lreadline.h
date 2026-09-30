#pragma once
/* The line editing hooks of the interactive prompt. user/Makefile
 * includes this header into third_party/lua/src/lua.c, which then uses
 * these four macros instead of reading lines with fgets (lua.c defines
 * its own only when lua_readline is undefined). lreadline.c implements
 * them over libedit. */
struct lua_State;

void minios_readline_init(struct lua_State *L);
char *minios_readline(const char *prompt);
void minios_readline_save(const char *line);

#define lua_initreadline(L) minios_readline_init(L)
#define lua_readline(buff, prompt) ((void)(buff), minios_readline(prompt))
#define lua_saveline(line) minios_readline_save(line)
#define lua_freeline(line) ((void)(line))
