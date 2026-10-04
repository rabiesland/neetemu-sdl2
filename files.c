#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "emu.h"
#include "build.h"

typedef struct { bool rd, wr, trunc, bin, create; } Mode;

static bool get_mode(const char *m, Mode *o) {
    if (!m || !*m || strlen(m) > 2) return false;
    char tmp[4];
    size_t n = strlen(m);
    if (n > 3) return false;
    for (size_t i = 0; i < n; i++) {
        char c = m[i];
        tmp[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    tmp[n] = 0;
    struct { const char *n; Mode v; } tab[] = {
        { "r",   { 1, 0, 0, 0, 0 } },
        { "w",   { 0, 1, 1, 0, 1 } },
        { "a",   { 0, 1, 0, 0, 1 } },
        { "r+",  { 1, 1, 0, 0, 0 } },
        { "w+",  { 1, 1, 1, 0, 1 } },
        { "a+",  { 1, 1, 0, 0, 1 } },
        { "rb",  { 1, 0, 0, 1, 0 } },
        { "wb",  { 0, 1, 1, 1, 1 } },
        { "ab",  { 0, 1, 0, 1, 1 } },
        { "rb+", { 1, 1, 0, 1, 0 } },
        { "r+b", { 1, 1, 0, 1, 0 } },
        { "wb+", { 1, 1, 1, 1, 1 } },
        { "w+b", { 1, 1, 1, 1, 1 } },
        { "ab+", { 1, 1, 0, 1, 1 } },
        { "a+b", { 1, 1, 0, 1, 1 } },
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (strcmp(tmp, tab[i].n) == 0) { *o = tab[i].v; return true; }
    return false;
}

static bool is_dir_path(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool path_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void mkdirs(const char *path) {
    char tmp[2048];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    for (char *c = tmp + 1; *c; c++) {
        if (*c == '/') {
            *c = 0;
            mkdir(tmp, 0755);
            *c = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void map_path(Emu *e, lua_State *L, const char *in,
                     char *out, size_t n) {
    char part[128] = "bios";
    const char *rest = in;
    const char *colon = strchr(in, ':');
    if (colon) {
        size_t plen = (size_t)(colon - in);
        if (plen == 0 || plen >= sizeof(part))
            luaL_error(L, "Invalid path '%s'", in);
        memcpy(part, in, plen);
        part[plen] = 0;
        for (char *c = part; *c; c++)
            if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                  (*c >= '0' && *c <= '9') || *c == '_' || *c == '-'))
                luaL_error(L, "Invalid path '%s'", in);
        rest = colon + 1;
    }

    char clean[1024];
    size_t w = 0;
    bool last_slash = true;
    for (const char *c = rest; *c && w + 1 < sizeof(clean); c++) {
        char ch = *c == '\\' ? '/' : *c;
        if (ch == '/') {
            if (last_slash) continue;
            last_slash = true;
            clean[w++] = '/';
        } else {
            last_slash = false;
            clean[w++] = ch;
        }
    }
    clean[w] = 0;
    if (strstr(clean, ".."))
        luaL_error(L, "Access denied");
    if (clean[0] == 0)
        snprintf(out, n, "%s/%s", e->disk_root, part);
    else
        snprintf(out, n, "%s/%s/%s", e->disk_root, part, clean);
}

static int disk_arg(lua_State *L, int idx) {
    if (lua_isnoneornil(L, idx)) return 0;
    if (!lua_isnumber(L, idx)) luaL_error(L, "Invalid disk");
    int d = (int)lua_tonumber(L, idx);
    if (d != 0) luaL_error(L, "Disk not found");
    return d;
}

typedef struct {
    bool open;
    Mode mode;
    unsigned char *buf;
    size_t len, cap;
    size_t cursor;
    char hostpath[2048];
} Header;

static Header *check_header(lua_State *L, bool need_open) {
    Header *h = (Header *)lua_touserdata(L, lua_upvalueindex(1));
    if (!h) luaL_error(L, "bad header");
    if (need_open && !h->open)
        luaL_error(L, "Attempt to use a closed file");
    return h;
}

static int header_gc(lua_State *L) {
    Header *h = (Header *)lua_touserdata(L, 1);

    free(h->buf);
    return 0;
}

static int h_flush(lua_State *L) {
    Header *h = check_header(L, true);
    if (h->mode.wr) {
        char dir[2048];
        strncpy(dir, h->hostpath, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = 0;
        char *sl = strrchr(dir, '/');
        if (sl) { *sl = 0; mkdirs(dir); }
        FILE *f = fopen(h->hostpath, "wb");
        if (!f) luaL_error(L, "flush failed");
        if (h->len) fwrite(h->buf, 1, h->len, f);
        fclose(f);
    }
    return 0;
}

static int h_close(lua_State *L) {
    Header *h = check_header(L, true);

    if (h->mode.wr) {
        char dir[2048];
        strncpy(dir, h->hostpath, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = 0;
        char *sl = strrchr(dir, '/');
        if (sl) { *sl = 0; mkdirs(dir); }
        FILE *f = fopen(h->hostpath, "wb");
        if (!f) luaL_error(L, "flush failed");
        if (h->len) fwrite(h->buf, 1, h->len, f);
        fclose(f);
    }
    h->open = false;
    free(h->buf);
    h->buf = NULL;
    h->len = h->cap = h->cursor = 0;
    return 0;
}

static int h_seek(lua_State *L) {
    Header *h = check_header(L, true);
    const char *whence = "cur";
    long offset = 0;
    if (!lua_isnoneornil(L, 1)) {
        if (!lua_isstring(L, 1)) luaL_error(L, "Invalid option");
        whence = lua_tostring(L, 1);
    }
    if (!lua_isnoneornil(L, 2)) {
        if (!lua_isnumber(L, 2)) luaL_error(L, "Invalid offset");
        offset = (long)lua_tonumber(L, 2);
    }
    if (strcmp(whence, "set") == 0) {
        if (offset < 0 || (size_t)offset > h->len)
            luaL_error(L, "Invalid offset");
        h->cursor = (size_t)offset;
    } else if (strcmp(whence, "cur") == 0) {
        long nc = (long)h->cursor + offset;
        if (nc < 0 || (size_t)nc > h->len)
            luaL_error(L, "Invalid offset");
        h->cursor = (size_t)nc;
    } else if (strcmp(whence, "end") == 0) {
        if (offset > 0 || (size_t)(-offset) > h->len)
            luaL_error(L, "Invalid offset");
        h->cursor = h->len - (size_t)(-offset);
    } else {
        luaL_error(L, "Invalid option");
    }
    lua_pushinteger(L, (lua_Integer)h->cursor);
    return 1;
}

static void h_append(Header *h, const void *data, size_t n) {
    if (h->len + n > h->cap) {
        size_t nc = h->cap ? h->cap * 2 : 256;
        while (nc < h->len + n) nc *= 2;
        h->buf = (unsigned char *)realloc(h->buf, nc);
        h->cap = nc;
    }
    memcpy(h->buf + h->len, data, n);
    h->len += n;
}

static int h_write(lua_State *L) {
    Header *h = check_header(L, true);
    if (!h->mode.wr)
        luaL_error(L, "Access denied");

    if (lua_type(L, 1) == LUA_TNUMBER) {

        unsigned char b = (unsigned char)((int)lua_tonumber(L, 1));
        h_append(h, &b, 1);
    } else if (lua_type(L, 1) == LUA_TSTRING) {
        size_t n = 0;
        const char *s = lua_tolstring(L, 1, &n);
        h_append(h, s, n);
    } else {
        luaL_error(L, "bad write arg");
    }
    return 0;
}

static int h_read(lua_State *L) {
    Header *h = check_header(L, true);
    if (!h->mode.rd)
        luaL_error(L, "Access denied");
    if (lua_type(L, 1) == LUA_TNUMBER) {

        long amount = (long)lua_tonumber(L, 1);
        long nc = (long)h->cursor + amount;
        if (nc < 0) nc = 0;
        if ((size_t)nc > h->len) nc = (long)h->len;
        if (nc == (long)h->cursor) {
            lua_pushstring(L, "");
            return 1;
        }
        size_t sp = nc < (long)h->cursor ? (size_t)nc : h->cursor;
        size_t ep = nc > (long)h->cursor ? (size_t)nc : h->cursor;
        lua_pushlstring(L, (const char *)h->buf + sp, ep - sp);
        h->cursor = (size_t)nc;
        return 1;
    }
    const char *fmt = "l";
    if (!lua_isnoneornil(L, 1)) {
        if (lua_type(L, 1) != LUA_TSTRING) luaL_error(L, "Invalid format");
        fmt = lua_tostring(L, 1);
        if (strlen(fmt) != 1) luaL_error(L, "Invalid format");
    }
    if (h->cursor == h->len) {
        lua_pushnil(L);
        return 1;
    }
    char f = fmt[0];
    if (f == 'l' || f == 'L') {
        size_t i = h->cursor;
        while (i < h->len && h->buf[i] != '\n') i++;
        if (i == h->len) {
            lua_pushlstring(L, (const char *)h->buf + h->cursor, h->len - h->cursor);
            h->cursor = h->len;
            return 1;
        }
        if (f == 'l')
            lua_pushlstring(L, (const char *)h->buf + h->cursor, i - h->cursor);
        else
            lua_pushlstring(L, (const char *)h->buf + h->cursor, i - h->cursor + 1);
        h->cursor = i + 1;
        return 1;
    }
    if (f == 'a') {
        lua_pushlstring(L, (const char *)h->buf + h->cursor, h->len - h->cursor);
        h->cursor = h->len;
        return 1;
    }
    luaL_error(L, "Invalid format");
    return 0;
}

static int f_open(lua_State *L) {
    Emu *e = emu_from(L);
    const char *path = lua_tostring(L, 1);
    if (!path) luaL_error(L, "bad path");
    const char *mode_s = "r";
    if (!lua_isnoneornil(L, 2)) {
        if (!lua_isstring(L, 2)) luaL_error(L, "Invalid open mode");
        mode_s = lua_tostring(L, 2);
    }
    disk_arg(L, 3);
    Mode m;
    char host[2048];
    map_path(e, L, path, host, sizeof(host));
    if (!get_mode(mode_s, &m))
        luaL_error(L, "Invalid open mode");
    bool isdir = is_dir_path(host);
    if (isdir)
        luaL_error(L, "Not a file");
    bool exists = path_exists(host);
    if (!exists && !m.create)
        luaL_error(L, "Not a file");

    Header *h = (Header *)lua_newuserdata(L, sizeof(Header));
    memset(h, 0, sizeof(*h));
    h->open = true;
    h->mode = m;
    strncpy(h->hostpath, host, sizeof(h->hostpath) - 1);
    if (luaL_newmetatable(L, "neetemu.header")) {
        lua_pushcfunction(L, header_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);

    if (exists && !m.trunc) {
        FILE *f = fopen(host, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0) {
                h->buf = (unsigned char *)malloc((size_t)sz);
                h->len = h->cap = fread(h->buf, 1, (size_t)sz, f);
            }
            fclose(f);
        }
    }
    if (m.create && !exists) {
        char dir[2048];
        strncpy(dir, host, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = 0;
        char *sl = strrchr(dir, '/');
        if (sl) { *sl = 0; mkdirs(dir); }
        FILE *f = fopen(host, "wb");
        if (f) fclose(f);
    }

    lua_newtable(L);

    static const luaL_Reg hf[] = {
        { "read", h_read }, { "write", h_write }, { "seek", h_seek },
        { "flush", h_flush }, { "close", h_close }, { NULL, NULL },
    };
    for (const luaL_Reg *r = hf; r->name; r++) {
        lua_pushvalue(L, -2);
        lua_pushcclosure(L, r->func, 1);
        lua_setfield(L, -2, r->name);
    }
    lua_remove(L, -2);
    return 1;
}

static int list_children(lua_State *L, const char *hostdir, const char *part) {
    (void)part;
    DIR *d = opendir(hostdir);
    lua_newtable(L);
    if (!d) return 1;
    struct dirent *de;
    int n = 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        lua_pushstring(L, de->d_name);
        lua_rawseti(L, -2, ++n);
    }
    closedir(d);
    return 1;
}

static int f_getPartitions(lua_State *L) {
    Emu *e = emu_from(L);
    disk_arg(L, 1);
    lua_newtable(L);
    int n = 0;

    char seen[BUILD_MAX_PARTS][128];
    int nseen = 0;
    DIR *d = opendir(e->disk_root);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char full[2048];
            snprintf(full, sizeof(full), "%s/%s", e->disk_root, de->d_name);
            if (is_dir_path(full)) {
                lua_pushstring(L, de->d_name);
                lua_rawseti(L, -2, ++n);
                if (nseen < BUILD_MAX_PARTS) {
                    strncpy(seen[nseen], de->d_name, sizeof(seen[0]) - 1);
                    seen[nseen][sizeof(seen[0]) - 1] = 0;
                    nseen++;
                }
            }
        }
        closedir(d);
    }
    BuildPart parts[BUILD_MAX_PARTS];
    int nparts = BUILD_MAX_PARTS;
    if (build_get_parts(e->disk_root, parts, &nparts)) {
        for (int i = 0; i < nparts; i++) {
            bool dup = false;
            for (int k = 0; k < nseen; k++)
                if (strcmp(seen[k], parts[i].path) == 0) { dup = true; break; }
            if (dup) continue;
            lua_pushstring(L, parts[i].path);
            lua_rawseti(L, -2, ++n);
            if (nseen < BUILD_MAX_PARTS) {
                strncpy(seen[nseen], parts[i].path, sizeof(seen[0]) - 1);
                seen[nseen][sizeof(seen[0]) - 1] = 0;
                nseen++;
            }
        }
    }
    return 1;
}

static int f_getPartition(lua_State *L) {
    Emu *e = emu_from(L);
    const char *name = lua_tostring(L, 1);
    disk_arg(L, 2);
    if (!name) { lua_pushnil(L); return 1; }
    bool readonly = false, hidden = false, known = false;
    BuildPart parts[BUILD_MAX_PARTS];
    int nparts = BUILD_MAX_PARTS;
    if (build_get_parts(e->disk_root, parts, &nparts)) {
        for (int i = 0; i < nparts; i++)
            if (strcmp(parts[i].path, name) == 0) {
                readonly = parts[i].readonly;
                hidden = parts[i].hidden;
                known = true;
                break;
            }
    }
    char full[2048];
    snprintf(full, sizeof(full), "%s/%s", e->disk_root, name);
    if (!is_dir_path(full) && !known) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushstring(L, name);
    lua_setfield(L, -2, "name");
    lua_pushboolean(L, readonly);
    lua_setfield(L, -2, "readonly");
    lua_pushboolean(L, hidden);
    lua_setfield(L, -2, "hidden");
    return 1;
}

static int f_createPartition(lua_State *L) {
    Emu *e = emu_from(L);
    const char *name = lua_tostring(L, 1);
    disk_arg(L, 2);
    if (!name || strchr(name, '/') || strchr(name, ':') || strstr(name, "..")) {
        lua_pushboolean(L, 0);
        return 1;
    }
    char full[2048];
    snprintf(full, sizeof(full), "%s/%s", e->disk_root, name);
    if (path_exists(full)) { lua_pushboolean(L, 0); return 1; }
    bool ok = mkdir(full, 0755) == 0;
    if (ok) build_add_partition(e->disk_root, name);
    lua_pushboolean(L, ok);
    return 1;
}

static int f_deletePartition(lua_State *L) {
    Emu *e = emu_from(L);
    const char *name = lua_tostring(L, 1);
    disk_arg(L, 2);
    if (!name) { lua_pushboolean(L, 0); return 1; }
    char full[2048];
    snprintf(full, sizeof(full), "%s/%s", e->disk_root, name);
    bool ok = rmdir(full) == 0;
    if (ok) build_remove_partition(e->disk_root, name);
    lua_pushboolean(L, ok);
    return 1;
}

static int f_setPartitionHidden(lua_State *L) {
    disk_arg(L, 3);
    lua_pushboolean(L, 1);
    return 1;
}

static int f_setPartitionReadOnly(lua_State *L) {
    disk_arg(L, 2);
    lua_pushboolean(L, 1);
    return 1;
}

static int f_getChildren(lua_State *L) {
    Emu *e = emu_from(L);
    const char *path = lua_tostring(L, 1);
    disk_arg(L, 2);
    if (!path) { lua_newtable(L); return 1; }
    char host[2048];

    char part[128] = "bios";
    const char *rest = path;
    const char *colon = strchr(path, ':');
    if (colon) {
        size_t plen = (size_t)(colon - path);
        if (plen >= sizeof(part)) { lua_newtable(L); return 1; }
        memcpy(part, path, plen);
        part[plen] = 0;
        rest = colon + 1;
    }
    char clean[1024];
    size_t w = 0;
    bool last_slash = true;
    for (const char *c = rest; *c && w + 1 < sizeof(clean); c++) {
        char ch = *c == '\\' ? '/' : *c;
        if (ch == '/') {
            if (last_slash) continue;
            last_slash = true;
            clean[w++] = '/';
        } else {
            last_slash = false;
            clean[w++] = ch;
        }
    }
    clean[w] = 0;
    if (strstr(clean, "..")) { lua_newtable(L); return 1; }
    if (clean[0] == 0)
        snprintf(host, sizeof(host), "%s/%s", e->disk_root, part);
    else
        snprintf(host, sizeof(host), "%s/%s/%s", e->disk_root, part, clean);
    return list_children(L, host, part);
}

static int f_makeDir(lua_State *L) {
    Emu *e = emu_from(L);
    const char *path = lua_tostring(L, 1);
    disk_arg(L, 2);
    if (!path) { lua_pushboolean(L, 0); return 1; }
    char host[2048];
    map_path(e, L, path, host, sizeof(host));
    mkdirs(host);
    lua_pushboolean(L, is_dir_path(host));
    return 1;
}

static int f_exists(lua_State *L) {
    Emu *e = emu_from(L);
    const char *path = lua_tostring(L, 1);
    disk_arg(L, 2);
    if (!path) { lua_pushboolean(L, 0); return 1; }
    char host[2048];

    if (strstr(path, "..")) { lua_pushboolean(L, 0); return 1; }
    map_path(e, L, path, host, sizeof(host));
    lua_pushboolean(L, path_exists(host));
    return 1;
}

static int f_isFile(lua_State *L) {
    Emu *e = emu_from(L);
    const char *path = lua_tostring(L, 1);
    disk_arg(L, 2);
    if (!path || strstr(path, "..")) { lua_pushboolean(L, 0); return 1; }
    char host[2048];
    map_path(e, L, path, host, sizeof(host));
    struct stat st;
    lua_pushboolean(L, stat(host, &st) == 0 && S_ISREG(st.st_mode));
    return 1;
}

static int f_isDir(lua_State *L) {
    Emu *e = emu_from(L);
    const char *path = lua_tostring(L, 1);
    disk_arg(L, 2);
    if (!path || strstr(path, "..")) { lua_pushboolean(L, 0); return 1; }
    char host[2048];
    map_path(e, L, path, host, sizeof(host));
    lua_pushboolean(L, is_dir_path(host));
    return 1;
}

static int f_delete(lua_State *L) {
    Emu *e = emu_from(L);
    const char *path = lua_tostring(L, 1);
    disk_arg(L, 2);
    if (!path || strstr(path, "..")) { lua_pushboolean(L, 0); return 1; }
    char boot[BUILD_MAX_ENTRY];
    if (build_get_boot(e->disk_root, boot, sizeof(boot)) &&
        build_same_path(path, boot))
        luaL_error(L, "Access denied");
    char host[2048];
    map_path(e, L, path, host, sizeof(host));
    bool ok = false;
    if (is_dir_path(host)) ok = rmdir(host) == 0;
    else ok = unlink(host) == 0;
    lua_pushboolean(L, ok);
    return 1;
}

static int f_getNumberOfDisks(lua_State *L) {
    lua_pushinteger(L, 1);
    return 1;
}

static int f_getDisks(lua_State *L) {
    lua_newtable(L);
    lua_pushinteger(L, 0);
    lua_rawseti(L, -2, 1);
    return 1;
}

static int f_getDiskID(lua_State *L) {
    disk_arg(L, 1);
    lua_pushstring(L, "emulator-disk-0");
    return 1;
}

static int f_removeDisk(lua_State *L) {
    disk_arg(L, 1);
    lua_pushboolean(L, 0);
    return 1;
}

static int f_getBootPath(lua_State *L) {
    Emu *e = emu_from(L);
    disk_arg(L, 1);
    char boot[BUILD_MAX_ENTRY];
    if (build_get_boot(e->disk_root, boot, sizeof(boot))) {
        lua_pushstring(L, boot);
        return 1;
    }

    char bios[2048];
    snprintf(bios, sizeof(bios), "%s/bios/bios.lua", e->disk_root);
    struct stat st;
    if (stat(bios, &st) == 0 && S_ISREG(st.st_mode)) {
        lua_pushstring(L, "bios:bios.lua");
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static int f_setBoot(lua_State *L) {
    Emu *e = emu_from(L);
    if (lua_isstring(L, 1)) {
        const char *entry = lua_tostring(L, 1);
        disk_arg(L, 2);
        char host[2048];
        if (!entry || !build_entry_to_host(e->disk_root, entry, host,
                                           sizeof(host))) {
            lua_pushboolean(L, 0);
            return 1;
        }
        struct stat st;
        if (stat(host, &st) != 0 || !S_ISREG(st.st_mode)) {
            lua_pushboolean(L, 0);
            return 1;
        }
        lua_pushboolean(L, build_set_boot(e->disk_root, entry));
        return 1;
    }

    disk_arg(L, 1);
    lua_pushboolean(L, 0);
    return 1;
}

void files_register(Emu *e) {
    static const luaL_Reg fns[] = {
        { "open", f_open },
        { "getPartitions", f_getPartitions },
        { "getPartition", f_getPartition },
        { "createPartition", f_createPartition },
        { "deletePartition", f_deletePartition },
        { "setPartitionHidden", f_setPartitionHidden },
        { "setPartitionReadOnly", f_setPartitionReadOnly },
        { "getChildren", f_getChildren },
        { "makeDir", f_makeDir },
        { "exists", f_exists },
        { "isFile", f_isFile },
        { "isDir", f_isDir },
        { "delete", f_delete },
        { "getNumberOfDisks", f_getNumberOfDisks },
        { "getDisks", f_getDisks },
        { "getDiskID", f_getDiskID },
        { "removeDisk", f_removeDisk },
        { "getBootPath", f_getBootPath },
        { "setBoot", f_setBoot },
        { NULL, NULL },
    };
    lua_newtable(e->L);
    luaL_setfuncs(e->L, fns, 0);
    lua_setglobal(e->L, "files");
}
