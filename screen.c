#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "emu.h"

int32_t nee_jrshift(int32_t v, int s) {
    if (s <= 0) return v;
    if (s >= 31) return v < 0 ? -1 : 0;
    if (v >= 0) return v >> s;
    int64_t d = (int64_t)1 << s;
    int64_t q = (int64_t)v / d;
    if ((int64_t)v % d != 0) q -= 1;
    return (int32_t)q;
}

uint32_t nee_pack(int r, int g, int b, int a) {
    return (((uint32_t)r & 0xFFu) << 24) |
           (((uint32_t)g & 0xFFu) << 16) |
           (((uint32_t)b & 0xFFu) << 8) |
           ((uint32_t)a & 0xFFu);
}

uint32_t nee_blend(uint32_t rgb, uint32_t rgba) {
    uint32_t A = rgba & 0xFFu;
    uint32_t src = (uint32_t)nee_jrshift((int32_t)rgba, 8);
    uint64_t t1 = (uint64_t)(src & 0xFF0000u) * A +
                  (uint64_t)(rgb & 0xFF0000u) * (255 - A);
    uint64_t t2 = (uint64_t)(src & 0xFF00u) * A +
                  (uint64_t)(rgb & 0xFF00u) * (255 - A);
    uint64_t t3 = (uint64_t)(src & 0xFFu) * A +
                  (uint64_t)(rgb & 0xFFu) * (255 - A);
    return ((uint32_t)(t1 / 255) & 0xFF0000u) |
           ((uint32_t)(t2 / 255) & 0xFF00u) |
           ((uint32_t)(t3 / 255) & 0xFFu);
}

static int check_int(lua_State *L, int idx, const char *need) {
    if (!lua_isnumber(L, idx))
        luaL_error(L, "#%d Expected number, got %s", idx,
                   lua_isnoneornil(L, idx) ? "nil" : luaL_typename(L, idx));
    (void)need;
    return (int)lua_tonumber(L, idx);
}

static int opt_alpha(lua_State *L, int idx) {
    if (lua_isnoneornil(L, idx)) return 255;
    if (!lua_isnumber(L, idx))
        luaL_error(L, "#%d Expected number, got %s", idx, luaL_typename(L, idx));
    return (int)lua_tonumber(L, idx);
}

static Screen *self_of(lua_State *L) {
    Screen **pp = (Screen **)lua_touserdata(L, lua_upvalueindex(1));
    return *pp;
}

static void raw_write_pixel(Screen *s, int x, int y, int r, int g, int b, int a) {
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) return;
    uint32_t rgba = nee_pack(r, g, b, a);
    uint32_t *cell = &s->px[x + y * s->w];
    if ((rgba & 0xFFu) == 255u) {
        *cell = (uint32_t)nee_jrshift((int32_t)rgba, 8);
        return;
    }
    *cell = nee_blend(*cell, rgba);
}

static int l_getSize(lua_State *L) {
    Screen *s = self_of(L);
    lua_pushinteger(L, s->w);
    lua_pushinteger(L, s->h);
    return 2;
}

static int l_writePixel(lua_State *L) {
    Screen *s = self_of(L);
    int x = check_int(L, 1, 0), y = check_int(L, 2, 0);
    int r = check_int(L, 3, 0), g = check_int(L, 4, 0), b = check_int(L, 5, 0);
    int a = opt_alpha(L, 6);
    raw_write_pixel(s, x, y, r, g, b, a);
    return 0;
}

static int l_readPixel(lua_State *L) {
    Screen *s = self_of(L);
    Emu *e = emu_from(L);
    int x = check_int(L, 1, 0), y = check_int(L, 2, 0);
    if (x < 0 || x >= s->w) {
        e->api_errors++;
        emu_api_note(e, "readPixel x=%d y=%d", x, y);
        luaL_error(L, "#1 Number %d not in range of 0-%d", x, s->w - 1);
    }
    if (y < 0 || y >= s->h) {
        e->api_errors++;
        emu_api_note(e, "readPixel x=%d y=%d", x, y);
        luaL_error(L, "#2 Number %d not in range of 0-%d", y, s->h - 1);
    }
    uint32_t rgb = s->px[x + y * s->w];

    lua_pushinteger(L, rgb & (0xFF0000u >> 16));
    lua_pushinteger(L, rgb & (0xFF00u >> 8));
    lua_pushinteger(L, rgb & 0xFFu);
    return 3;
}

static int l_writeLine(lua_State *L) {
    Screen *s = self_of(L);
    int x1 = check_int(L, 1, 0), y1 = check_int(L, 2, 0);
    int x2 = check_int(L, 3, 0), y2 = check_int(L, 4, 0);
    int r = check_int(L, 5, 0), g = check_int(L, 6, 0), b = check_int(L, 7, 0);
    int a = opt_alpha(L, 8);

    int dx = x2 - x1, dy = y2 - y1;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int step = adx > ady ? adx : ady;
    if (step == 0) {
        raw_write_pixel(s, x1, y1, r, g, b, a);
        return 0;
    }
    float x_incr = (float)dx / (float)step;
    float y_incr = (float)dy / (float)step;
    float x = (float)x1, y = (float)y1;
    for (int i = 0; i <= step; i++) {

        raw_write_pixel(s, (int)floorf(x + 0.5f), (int)floorf(y + 0.5f),
                        r, g, b, a);
        x += x_incr;
        y += y_incr;
    }
    return 0;
}

static int l_substitute(lua_State *L) {
    Screen *s = self_of(L);
    int r1 = check_int(L, 1, 0), g1 = check_int(L, 2, 0), b1 = check_int(L, 3, 0);
    int r2 = check_int(L, 4, 0), g2 = check_int(L, 5, 0), b2 = check_int(L, 6, 0);
    uint32_t src = (uint32_t)nee_jrshift((int32_t)nee_pack(r1, g1, b1, 0), 8);
    uint32_t dst = (uint32_t)nee_jrshift((int32_t)nee_pack(r2, g2, b2, 0), 8);
    size_t n = (size_t)s->w * (size_t)s->h;
    for (size_t i = 0; i < n; i++)
        if (s->px[i] == src) s->px[i] = dst;
    return 0;
}

static void check_sector(lua_State *L, Emu *e, Screen *s,
                         int x1, int y1, int x2, int y2, const char *fn) {
    if (x1 > x2) {
        e->api_errors++;
        emu_api_note(e, "%s x1=%d y1=%d x2=%d y2=%d", fn, x1, y1, x2, y2);
        luaL_error(L, "x2 must be larger then x1");
    }
    if (y1 > y2) {
        e->api_errors++;
        emu_api_note(e, "%s x1=%d y1=%d x2=%d y2=%d", fn, x1, y1, x2, y2);
        luaL_error(L, "y2 must be larger then y1");
    }
    if (x1 < 0) {
        e->api_errors++;
        emu_api_note(e, "%s x1=%d y1=%d x2=%d y2=%d", fn, x1, y1, x2, y2);
        luaL_error(L, "#1 Number %d not in range of 0-%d", x1, s->w - 1);
    }
    if (y1 < 0) {
        e->api_errors++;
        emu_api_note(e, "%s x1=%d y1=%d x2=%d y2=%d", fn, x1, y1, x2, y2);
        luaL_error(L, "#2 Number %d not in range of 0-%d", y1, s->h - 1);
    }
    if (x2 >= s->w) {
        e->api_errors++;
        emu_api_note(e, "%s x1=%d y1=%d x2=%d y2=%d", fn, x1, y1, x2, y2);
        luaL_error(L, "#3 Number %d not in range of 0-%d", x2, s->w - 1);
    }
    if (y2 >= s->h) {
        e->api_errors++;
        emu_api_note(e, "%s x1=%d y1=%d x2=%d y2=%d", fn, x1, y1, x2, y2);
        luaL_error(L, "#4 Number %d not in range of 0-%d", y2, s->h - 1);
    }
}

static int l_fill(lua_State *L) {
    Screen *s = self_of(L);
    Emu *e = emu_from(L);
    int x1 = check_int(L, 1, 0), y1 = check_int(L, 2, 0);
    int x2 = check_int(L, 3, 0), y2 = check_int(L, 4, 0);
    int r = check_int(L, 5, 0), g = check_int(L, 6, 0), b = check_int(L, 7, 0);
    int a = opt_alpha(L, 8);
    check_sector(L, e, s, x1, y1, x2, y2, "fill");
    uint32_t rgba = nee_pack(r, g, b, a);
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    if ((rgba & 0xFFu) == 255u) {
        uint32_t c = (uint32_t)nee_jrshift((int32_t)rgba, 8);
        for (int j = 0; j < h; j++) {
            uint32_t *row = &s->px[(y1 + j) * s->w + x1];
            for (int i = 0; i < w; i++) row[i] = c;
        }
    } else {
        for (int j = 0; j < h; j++) {
            uint32_t *row = &s->px[(y1 + j) * s->w + x1];
            for (int i = 0; i < w; i++) row[i] = nee_blend(row[i], rgba);
        }
    }
    return 0;
}

static int l_clone(lua_State *L) {
    Screen *s = self_of(L);
    Emu *e = emu_from(L);
    int x1 = check_int(L, 1, 0), y1 = check_int(L, 2, 0);
    int x2 = check_int(L, 3, 0), y2 = check_int(L, 4, 0);
    check_sector(L, e, s, x1, y1, x2, y2, "clone");
    Screen *n = (Screen *)calloc(1, sizeof(Screen));
    if (!n) luaL_error(L, "out of memory");
    n->w = x2 - x1 + 1;
    n->h = y2 - y1 + 1;
    n->owned = true;
    n->px = (uint32_t *)malloc((size_t)n->w * (size_t)n->h * 4);
    if (!n->px) { free(n); luaL_error(L, "out of memory"); }
    for (int j = 0; j < n->h; j++)
        memcpy(&n->px[j * n->w], &s->px[(y1 + j) * s->w + x1],
               (size_t)n->w * 4);
    screen_push_table(L, e, n);
    return 1;
}

static int l_readData(lua_State *L) {
    Screen *s = self_of(L);
    Emu *e = emu_from(L);
    int x1 = check_int(L, 1, 0), y1 = check_int(L, 2, 0);
    int x2 = check_int(L, 3, 0), y2 = check_int(L, 4, 0);
    check_sector(L, e, s, x1, y1, x2, y2, "readData");

    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int j = y1; j <= y2; j++) {
        for (int i = x1; i <= x2; i++) {
            uint32_t v = s->px[i + j * s->w];
            char tmp[4];
            tmp[0] = (char)((v >> 16) & 0xFF);
            tmp[1] = (char)((v >> 8) & 0xFF);
            tmp[2] = (char)(v & 0xFF);
            tmp[3] = (char)0xFF;
            luaL_addlstring(&b, tmp, 4);
        }
    }
    luaL_pushresult(&b);
    return 1;
}

static int l_writeData(lua_State *L) {
    Screen *s = self_of(L);
    Emu *e = emu_from(L);
    int x = check_int(L, 1, 0), y = check_int(L, 2, 0);
    size_t blen = 0;
    if (lua_type(L, 3) != LUA_TSTRING)
        luaL_error(L, "#3 Expected string, got %s",
                   lua_isnoneornil(L, 3) ? "nil" : luaL_typename(L, 3));
    const char *buf = lua_tolstring(L, 3, &blen);
    int w = check_int(L, 4, 0);
    if (blen % 4 != 0) {
        e->api_errors++;
        emu_api_note(e, "writeData x=%d y=%d len=%zu w=%d", x, y, blen, w);
        luaL_error(L, "Length of buffer must by dividable by 4");
    }
    if (w <= 0 || (blen / 4) % (size_t)w != 0) {
        e->api_errors++;
        emu_api_note(e, "writeData x=%d y=%d len=%zu w=%d", x, y, blen, w);
        luaL_error(L, "Length of buffer must by dividable by width");
    }
    int h = (int)(blen / (size_t)w / 4);

    if (x < 0) {
        e->api_errors++;
        emu_api_note(e, "writeData x=%d y=%d len=%zu w=%d", x, y, blen, w);
        luaL_error(L, "#1 Number %d not in range of 0-%d", x, w - 1);
    }
    if (y < 0) {
        e->api_errors++;
        emu_api_note(e, "writeData x=%d y=%d len=%zu w=%d", x, y, blen, w);
        luaL_error(L, "#2 Number %d not in range of 0-%d", y, h - 1);
    }
    if (y + h > s->h || x + w > s->w) {
        e->api_errors++;
        emu_api_note(e, "writeData x=%d y=%d len=%zu w=%d", x, y, blen, w);
        luaL_error(L, "Draw call extends past valid bounds");
    }
    const unsigned char *ub = (const unsigned char *)buf;
    for (int j = 0; j < h; j++) {
        uint32_t *row = &s->px[(y + j) * s->w + x];
        for (int i = 0; i < w; i++) {
            size_t k = ((size_t)j * (size_t)w + (size_t)i) * 4;
            unsigned r = ub[k], g = ub[k + 1], bb = ub[k + 2], a = ub[k + 3];
            if (a == 255) {
                row[i] = (r << 16) | (g << 8) | bb;
            } else if (a != 0) {
                uint32_t rgba = nee_pack((int)r, (int)g, (int)bb, (int)a);
                row[i] = nee_blend(row[i], rgba);
            }
        }
    }
    return 0;
}

static int l_set(lua_State *L) {
    Screen *s = self_of(L);
    if (lua_gettop(L) == 0) {
        memset(s->px, 0, (size_t)s->w * (size_t)s->h * 4);
        return 0;
    }
    int r = check_int(L, 1, 0), g = check_int(L, 2, 0), b = check_int(L, 3, 0);
    int a = opt_alpha(L, 4);
    uint32_t rgba = nee_pack(r, g, b, a);
    if (rgba == 0xFFu) {
        memset(s->px, 0, (size_t)s->w * (size_t)s->h * 4);
        return 0;
    }
    if ((rgba & 0xFFu) == 255u) {
        uint32_t c = (uint32_t)nee_jrshift((int32_t)rgba, 8);
        size_t n = (size_t)s->w * (size_t)s->h;
        for (size_t i = 0; i < n; i++) s->px[i] = c;
        return 0;
    }
    size_t n = (size_t)s->w * (size_t)s->h;
    for (size_t i = 0; i < n; i++) s->px[i] = nee_blend(s->px[i], rgba);
    return 0;
}

static int l_createLayer(lua_State *L) {
    Emu *e = emu_from(L);
    int sx = check_int(L, 1, 0), sy = check_int(L, 2, 0);
    if (sx <= 0 || sy <= 0) {
        e->api_errors++;
        emu_api_note(e, "createLayer %d x %d", sx, sy);
        luaL_error(L, "Size cant be zero or less");
    }
    Screen *n = (Screen *)calloc(1, sizeof(Screen));
    if (!n) luaL_error(L, "out of memory");
    n->w = sx;
    n->h = sy;
    n->owned = true;
    n->px = (uint32_t *)calloc((size_t)sx * (size_t)sy, 4);
    if (!n->px) { free(n); luaL_error(L, "out of memory"); }
    screen_push_table(L, e, n);
    return 1;
}

static int l_draw(lua_State *L) {
    Emu *e = emu_from(L);

    if (e->present) e->present(e);
    e->presents++;
    return 0;
}

static int screen_gc(lua_State *L) {
    Screen **pp = (Screen **)lua_touserdata(L, 1);
    if (pp && *pp && (*pp)->owned) {
        free((*pp)->px);
        free(*pp);
    }
    return 0;
}

typedef struct { const char *name; lua_CFunction fn; int drawable_only; } SFun;

static const SFun s_funs[] = {
    { "getSize", l_getSize, 0 },
    { "writePixel", l_writePixel, 0 },
    { "readPixel", l_readPixel, 0 },
    { "writeLine", l_writeLine, 0 },
    { "substitute", l_substitute, 0 },
    { "clone", l_clone, 0 },
    { "readData", l_readData, 0 },
    { "writeData", l_writeData, 0 },
    { "set", l_set, 0 },
    { "fill", l_fill, 0 },
    { "createLayer", l_createLayer, 0 },
    { "draw", l_draw, 1 },
    { NULL, NULL, 0 },
};

void screen_push_table(lua_State *L, Emu *e, Screen *s) {
    (void)e;
    Screen **pp = (Screen **)lua_newuserdata(L, sizeof(Screen *));
    *pp = s;
    if (luaL_newmetatable(L, "neetemu.screen")) {
        lua_pushcfunction(L, screen_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);

    lua_newtable(L);
    for (const SFun *f = s_funs; f->name; f++) {
        if (f->drawable_only && !s->drawable) continue;
        lua_pushvalue(L, -2);
        lua_pushcclosure(L, f->fn, 1);
        lua_setfield(L, -2, f->name);
    }
    lua_remove(L, -2);
}

void screen_register(Emu *e) {

    e->mainscreen.w = NEET_W;
    e->mainscreen.h = NEET_H;
    e->mainscreen.drawable = true;
    e->mainscreen.owned = false;
    if (!e->mainscreen.px)
        e->mainscreen.px = (uint32_t *)calloc(NEET_W * NEET_H, 4);

    memset(e->mainscreen.px, 0, NEET_W * NEET_H * 4);
    screen_push_table(e->L, e, &e->mainscreen);
    lua_setglobal(e->L, "screen");
}
