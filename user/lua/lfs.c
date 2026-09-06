/* The fs module: directory listing, file status and the directory
 * operations that the standard io and os libraries do not provide.
 * Failures return nil, the message and the errno, like io.open. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "lauxlib.h"
#include "minios.h"

/* The host unit test compiles with strict POSIX feature selection, which
 * hides the entry type constants; the values agree on every system. */
#ifndef DT_DIR
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#endif

int minios_errresult(lua_State *L)
{
    int err = errno;
    lua_pushnil(L);
    lua_pushstring(L, strerror(err));
    lua_pushinteger(L, err);
    return 3;
}

static const char *type_name(unsigned mode)
{
    switch (mode & S_IFMT) {
    case S_IFDIR: return "directory";
    case S_IFREG: return "file";
    case S_IFCHR: return "char";
    case S_IFBLK: return "block";
    case S_IFIFO: return "fifo";
    }
    return "unknown";
}

static const char *dirent_type_name(unsigned type)
{
    switch (type) {
    case DT_DIR: return "directory";
    case DT_REG: return "file";
    case DT_CHR: return "char";
    case DT_BLK: return "block";
    case DT_FIFO: return "fifo";
    }
    return "unknown";
}

static int dot_entry(const char *name)
{
    return name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0));
}

/* ---- fs.dir: an iterator over name, type ---- */

#define DIR_META "fs.dir"

struct dir_handle {
    DIR *d;
};

static int dir_gc(lua_State *L)
{
    struct dir_handle *h = luaL_checkudata(L, 1, DIR_META);
    if (h->d)
        closedir(h->d);
    h->d = NULL;
    return 0;
}

static int dir_next(lua_State *L)
{
    struct dir_handle *h = luaL_checkudata(L, lua_upvalueindex(1), DIR_META);
    if (!h->d)
        return 0;
    struct dirent *e;
    do {
        errno = 0;
        e = readdir(h->d);
    } while (e && dot_entry(e->d_name));
    if (!e) {
        if (errno)
            return luaL_error(L, "readdir: %s", strerror(errno));
        closedir(h->d);
        h->d = NULL;
        return 0;
    }
    lua_pushstring(L, e->d_name);
    lua_pushstring(L, dirent_type_name(e->d_type));
    return 2;
}

static int fs_dir(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    DIR *d = opendir(path);
    if (!d)
        return luaL_error(L, "%s: %s", path, strerror(errno));
    struct dir_handle *h = lua_newuserdatauv(L, sizeof *h, 0);
    h->d = d;
    luaL_setmetatable(L, DIR_META);
    lua_pushcclosure(L, dir_next, 1);
    return 1;
}

/* ---- fs.list: a sorted array of names ---- */

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int fs_list(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    DIR *d = opendir(path);
    if (!d)
        return minios_errresult(L);
    char **names = NULL;
    int count = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (dot_entry(e->d_name))
            continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 32;
            char **grown = realloc(names, (size_t)cap * sizeof *names);
            if (!grown)
                break;
            names = grown;
        }
        names[count] = strdup(e->d_name);
        if (!names[count])
            break;
        count++;
    }
    closedir(d);
    qsort(names, (size_t)count, sizeof *names, compare_names);
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; i++) {
        lua_pushstring(L, names[i]);
        lua_rawseti(L, -2, i + 1);
        free(names[i]);
    }
    free(names);
    return 1;
}

/* ---- status ---- */

static void push_stat(lua_State *L, const struct stat *st)
{
    lua_createtable(L, 0, 10);
    lua_pushstring(L, type_name(st->st_mode));
    lua_setfield(L, -2, "type");
    lua_pushinteger(L, st->st_size);
    lua_setfield(L, -2, "size");
    lua_pushinteger(L, st->st_mtime);
    lua_setfield(L, -2, "mtime");
    lua_pushinteger(L, (lua_Integer)st->st_mode);
    lua_setfield(L, -2, "mode");
    lua_pushinteger(L, (lua_Integer)st->st_ino);
    lua_setfield(L, -2, "inode");
    lua_pushinteger(L, (lua_Integer)st->st_dev);
    lua_setfield(L, -2, "device");
    lua_pushinteger(L, (lua_Integer)st->st_nlink);
    lua_setfield(L, -2, "links");
    lua_pushinteger(L, st->st_blocks);
    lua_setfield(L, -2, "blocks");
    lua_pushinteger(L, st->st_blksize);
    lua_setfield(L, -2, "blocksize");
}

static int fs_stat(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    struct stat st;
    if (stat(path, &st) < 0)
        return minios_errresult(L);
    push_stat(L, &st);
    return 1;
}

static int fs_exists(lua_State *L)
{
    struct stat st;
    lua_pushboolean(L, stat(luaL_checkstring(L, 1), &st) == 0);
    return 1;
}

/* ---- directory operations ---- */

static int result(lua_State *L, int rc)
{
    if (rc < 0)
        return minios_errresult(L);
    lua_pushboolean(L, 1);
    return 1;
}

static int fs_mkdir(lua_State *L)
{
    return result(L, mkdir(luaL_checkstring(L, 1), (mode_t)luaL_optinteger(L, 2, 0755)));
}

static int fs_rmdir(lua_State *L)
{
    return result(L, rmdir(luaL_checkstring(L, 1)));
}

static int fs_chdir(lua_State *L)
{
    return result(L, chdir(luaL_checkstring(L, 1)));
}

static int fs_getcwd(lua_State *L)
{
    char buf[512];
    if (!getcwd(buf, sizeof buf))
        return minios_errresult(L);
    lua_pushstring(L, buf);
    return 1;
}

static int fs_sync(lua_State *L)
{
    sync();
    return 0;
}

/* ---- whole files ---- */

static int fs_read(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    FILE *f = fopen(path, "rb");
    if (!f)
        return minios_errresult(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (;;) {
        char *p = luaL_prepbuffsize(&b, LUAL_BUFFERSIZE);
        size_t n = fread(p, 1, LUAL_BUFFERSIZE, f);
        luaL_addsize(&b, n);
        if (n < LUAL_BUFFERSIZE)
            break;
    }
    int failed = ferror(f);
    fclose(f);
    if (failed)
        return minios_errresult(L);
    luaL_pushresult(&b);
    return 1;
}

static int fs_write(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    FILE *f = fopen(path, lua_toboolean(L, 3) ? "ab" : "wb");
    if (!f)
        return minios_errresult(L);
    int ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0)
        ok = 0;
    if (!ok)
        return minios_errresult(L);
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg fs_funcs[] = {
    { "dir", fs_dir },
    { "list", fs_list },
    { "stat", fs_stat },
    { "exists", fs_exists },
    { "mkdir", fs_mkdir },
    { "rmdir", fs_rmdir },
    { "chdir", fs_chdir },
    { "getcwd", fs_getcwd },
    { "sync", fs_sync },
    { "read", fs_read },
    { "write", fs_write },
    { NULL, NULL }
};

int luaopen_fs(lua_State *L)
{
    luaL_newmetatable(L, DIR_META);
    lua_pushcfunction(L, dir_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    luaL_newlib(L, fs_funcs);
    return 1;
}
