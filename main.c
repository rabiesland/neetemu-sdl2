#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include <dlfcn.h>

#include "lua.h"
#include "lauxlib.h"
#include "emu.h"

#ifndef NEETEMU_VERSION
#define NEETEMU_VERSION "dev"
#endif

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static uint32_t *g_argb;

typedef struct {
    bool headless;
    long tick_cap;
    double timeout;
    bool smoke;
    bool clickall;
    bool watchdog_test;
    bool reboot_test;
    const char *app;
    const char *disk;
    bool app_given;
    bool disk_given;
    bool show_version;
    bool list_computers;
    long computer_id;
    int zoom;
    int with_n3d;
    const char *n3d_lib;
    const char *app_args[32];
    int app_argc;
} Opts;

static void do_present(Emu *e) {
    uint32_t *src = e->mainscreen.px;
    for (int i = 0; i < NEET_W * NEET_H; i++)
        g_argb[i] = 0xFF000000u | src[i];
    if (e->overlay)
        overlay_compose(e, g_argb);
    if (e->show_help)
        overlay_help(e, g_argb);
    if (g_tex) {
        SDL_UpdateTexture(g_tex, NULL, g_argb, NEET_W * sizeof(uint32_t));
        SDL_RenderClear(g_ren);
        SDL_RenderTexture(g_ren, g_tex, NULL, NULL);
        SDL_RenderPresent(g_ren);
    }
}

static int next_snap_no(void) {
    int best = -1;
    for (int i = 0; i < 100000; i++) {
        char path[96];
        snprintf(path, sizeof(path), "snapshots/snap_%04d.bmp", i);
        if (access(path, F_OK) == 0) best = i;
        else if (i > best + 1) break;
    }
    return best + 1;
}

static void snapshot(Emu *e) {
    mkdir("snapshots", 0755);
    int no = next_snap_no();
    char path[96];
    snprintf(path, sizeof(path), "snapshots/snap_%04d.bmp", no);

    do_present(e);
    SDL_Surface *s = SDL_CreateSurfaceFrom(NEET_W, NEET_H,
                                           SDL_PIXELFORMAT_ARGB8888,
                                           g_argb, NEET_W * 4);
    if (!s) {
        emu_log("snapshot failed: %s", SDL_GetError());
        return;
    }
    if (SDL_SaveBMP(s, path))
        emu_log("snapshot -> %s", path);
    else
        emu_log("snapshot failed: %s", SDL_GetError());
    SDL_DestroySurface(s);
}

static void fb_map(Emu *e, float wx, float wy, int *lx, int *ly) {
    (void)e;
    if (g_ren) {
        float fx, fy;
        if (SDL_RenderCoordinatesFromWindow(g_ren, wx, wy, &fx, &fy)
            && fx >= 0.0f && fx < (float)NEET_W
            && fy >= 0.0f && fy < (float)NEET_H) {
            *lx = (int)fx;
            *ly = (int)fy;
            return;
        }
        *lx = -1;
        *ly = -1;
        return;
    }
    int w = 800, h = 600;
    if (g_win) SDL_GetWindowSize(g_win, &w, &h);
    if (w <= 0) w = 800;
    if (h <= 0) h = 600;
    *lx = (int)(wx * NEET_W / w);
    *ly = (int)(wy * NEET_H / h);
}

static bool inside(int x, int y) {
    return x >= 0 && x < NEET_W && y >= 0 && y < NEET_H;
}

static int sdl_btn_to_neet(int b) {
    switch (b) {
    case SDL_BUTTON_LEFT: return 0;
    case SDL_BUTTON_RIGHT: return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    case SDL_BUTTON_X1: return 3;
    case SDL_BUTTON_X2: return 4;
    default: return b - 1;
    }
}

static void in_motion(Emu *e, int x, int y) {
    bool was_in = inside(e->mouse_x, e->mouse_y);
    bool now_in = inside(x, y);
    e->mouse_x = x;
    e->mouse_y = y;
    if (now_in) {
        if (x != e->last_moved_x || y != e->last_moved_y) {
            ev_push_move(e, x, y);
            e->last_moved_x = x;
            e->last_moved_y = y;
        }
        for (int k = 0; k < 8; k++) {
            if (!e->btn[k].held) continue;
            if (e->btn[k].exited) {

                ev_push_mouse(e, "mouseClicked", x, y, k);
                e->btn[k].exited = false;
                e->btn[k].x = x;
                e->btn[k].y = y;
            }
            if (x != e->btn[k].x || y != e->btn[k].y) {
                ev_push_mouse(e, "mouseDragged", x, y, k);
                e->btn[k].x = x;
                e->btn[k].y = y;
            }
        }
    } else if (was_in) {

        for (int k = 0; k < 8; k++) {
            if (!e->btn[k].held || e->btn[k].exited) continue;
            ev_push_mouse(e, "mouseReleased", e->btn[k].x, e->btn[k].y, k);
            e->btn[k].exited = true;
        }
    }
}

static void in_button(Emu *e, int x, int y, int key, bool down) {
    if (key < 0 || key >= 8) return;
    if (down) {
        if (!inside(x, y)) return;
        e->mouse_x = x;
        e->mouse_y = y;
        ev_push_mouse(e, "mouseClicked", x, y, key);
        e->btn[key].held = true;
        e->btn[key].exited = false;
        e->btn[key].x = x;
        e->btn[key].y = y;
    } else {
        if (inside(x, y)) {
            e->mouse_x = x;
            e->mouse_y = y;
            ev_push_mouse(e, "mouseReleased", x, y, key);
            e->btn[key].held = false;
            e->btn[key].exited = false;
        } else {

            e->btn[key].held = false;
            e->btn[key].exited = false;
        }
    }
}

static int neet_key(SDL_Keycode k, SDL_Keymod mod, char *letter) {
    bool shift = (mod & SDL_KMOD_SHIFT) != 0;
    letter[0] = 0;
    int code = 0;
    if (k >= SDLK_A && k <= SDLK_Z) {
        bool up = shift || (mod & SDL_KMOD_CAPS);
        code = up ? (int)(k - 32) : (int)k;
    } else if (k >= SDLK_0 && k <= SDLK_9) {
        if (shift) {
            static const char *m = ")!@#$%^&*(";
            int d = (k == SDLK_0) ? 0 : (int)(k - SDLK_1 + 1);
            code = m[d];
        } else code = (int)k;
    } else if (k >= 32 && k <= 126) {

        switch ((int)k) {
        case '\'': code = shift ? '"' : '\''; break;
        case ',': code = shift ? '<' : ','; break;
        case '-': code = shift ? '_' : '-'; break;
        case '.': code = shift ? '>' : '.'; break;
        case '/': code = shift ? '?' : '/'; break;
        case ';': code = shift ? ':' : ';'; break;
        case '=': code = shift ? '+' : '='; break;
        case '[': code = shift ? '{' : '['; break;
        case '\\': code = shift ? '|' : '\\'; break;
        case ']': code = shift ? '}' : ']'; break;
        case '`': code = shift ? '~' : '`'; break;
        case ' ': code = ' '; break;
        default: code = (int)k; break;
        }
    } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) code = 13;
    else if (k == SDLK_TAB) code = 9;
    else if (k == SDLK_BACKSPACE) code = 8;
    else if (k == SDLK_DELETE) code = 127;
    else if (k == SDLK_ESCAPE) code = 27;
    else if (k == SDLK_LEFT) code = 128;
    else if (k == SDLK_RIGHT) code = 129;
    else if (k == SDLK_UP) code = 130;
    else if (k == SDLK_DOWN) code = 131;
    else if (k == SDLK_HOME) code = 158;
    else if (k == SDLK_END) code = 159;
    else if (k == SDLK_PAGEUP) code = 160;
    else if (k == SDLK_PAGEDOWN) code = 161;
    else if (k >= SDLK_F1 && k <= SDLK_F12) code = 134 + (int)(k - SDLK_F1);
    else if (k >= SDLK_F13 && k <= SDLK_F24)
        code = 134 + 12 + (int)(k - SDLK_F13);
    else if (k == SDLK_LSHIFT || k == SDLK_RSHIFT) code = 14;
    else if (k == SDLK_LCTRL || k == SDLK_RCTRL) code = 132;
    else if (k == SDLK_LALT || k == SDLK_RALT) code = 133;
    else return 0;
    if (code > 31 && code < 128) {
        letter[0] = (char)code;
        letter[1] = 0;
    }
    return code;
}

static int neet_mods(SDL_Keymod m) {
    return ((m & SDL_KMOD_SHIFT) ? 1 : 0) | ((m & SDL_KMOD_CTRL) ? 2 : 0) |
           ((m & SDL_KMOD_ALT) ? 4 : 0) | ((m & SDL_KMOD_GUI) ? 8 : 0);
}

static bool pump_input(Emu *e, const Opts *o) {
    (void)o;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_QUIT:
            return false;
        case SDL_EVENT_MOUSE_MOTION: {
            int fx, fy;
            fb_map(e, ev.motion.x, ev.motion.y, &fx, &fy);
            in_motion(e, fx, fy);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            bool down = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            int k = sdl_btn_to_neet(ev.button.button);
            int fx, fy;
            fb_map(e, ev.button.x, ev.button.y, &fx, &fy);
            in_button(e, fx, fy, k, down);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            float wx = 0, wy = 0;
            SDL_GetMouseState(&wx, &wy);
            int x, y;
            fb_map(e, wx, wy, &x, &y);
            double h = ev.wheel.x, v = ev.wheel.y;
            if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                h = -h;
                v = -v;
            }
            if (inside(x, y)) ev_push_scroll(e, x, y, h, v);
            break;
        }
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            bool down = ev.type == SDL_EVENT_KEY_DOWN;
            if (down && ev.key.repeat) break;
            SDL_Keycode k = ev.key.key;

            if (k == SDLK_1 || k == SDLK_2) {
                if ((ev.key.mod & SDL_KMOD_CTRL) != 0) {
                    if (down) {
                        int z = (k == SDLK_1) ? 1 : 2;
                        ((Emu *)e)->zoom = z;
                        if (g_win) SDL_SetWindowSize(g_win, NEET_W * z, NEET_H * z);
                        emu_log("zoom %dx", z);
                    }
                    break;
                }
            }
            if (down && k == SDLK_F9) { snapshot(e); break; }
            if (down && k == SDLK_F10) {
                emu_log("F10: reboot");
                ((Emu *)e)->show_help = false;
                emu_boot(e);
                break;
            }
            if (down && k == SDLK_F1) {
                ((Emu *)e)->show_help = !((Emu *)e)->show_help;
                emu_log("help %s (F1)",
                        ((Emu *)e)->show_help ? "on" : "off");
                break;
            }
            if (down && k == SDLK_F11) {
                Uint32 flags = SDL_GetWindowFlags(g_win);
                bool fs = !(flags & SDL_WINDOW_FULLSCREEN);
                SDL_SetWindowFullscreen(g_win, fs);
                emu_log("fullscreen %s (F11)", fs ? "on" : "off");
                break;
            }
            char letter[2] = { 0, 0 };
            int code = neet_key(k, ev.key.mod, letter);
            if (code != 0)
                ev_push_key(e, down ? "keyPressed" : "keyReleased",
                            code, letter, neet_mods(ev.key.mod));
            break;
        }
        default:
            break;
        }
    }
    return true;
}

static bool run_ticks(Emu *e, long n, double deadline) {
    for (long i = 0; i < n; i++) {
        if (e->state != EMU_RUN) return true;
        if (deadline > 0 && emu_now() > deadline) {
            emu_log("timeout reached");
            return false;
        }
        SDL_PumpEvents();
        SDL_Event ev;
        while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_EVENT_QUIT,
                              SDL_EVENT_QUIT) > 0)
            return false;
        emu_step(e);
        if (e->reboot_requested) emu_boot(e);
    }
    return true;
}

static int do_smoke(Emu *e, double deadline) {
    emu_log("smoke: settling 5 ticks");
    if (!run_ticks(e, 5, deadline)) return 1;
    long before = e->events_in;

    int pts[7][2] = { { 100, 100 }, { 300, 120 }, { 500, 200 }, { 700, 300 },
                      { 200, 400 }, { 400, 300 }, { 600, 450 } };
    for (int i = 0; i < 7; i++) {
        in_motion(e, pts[i][0], pts[i][1]);
        emu_step(e);
        in_button(e, pts[i][0], pts[i][1], 0, true);
        emu_step(e);
        in_button(e, pts[i][0], pts[i][1], 0, false);
        emu_step(e);
        if (e->state != EMU_RUN) break;
    }

    in_button(e, 300, 300, 0, true);
    emu_step(e);
    for (int i = 1; i <= 5; i++) {
        in_motion(e, 300 + i * 20, 300 + i * 10);
        emu_step(e);
    }
    in_button(e, 400, 350, 0, false);
    emu_step(e);

    in_button(e, 500, 300, 1, true);
    emu_step(e);
    for (int i = 1; i <= 5; i++) {
        in_motion(e, 500 - i * 15, 300 + i * 8);
        emu_step(e);
    }
    in_button(e, 425, 340, 1, false);
    emu_step(e);

    ev_push_scroll(e, 400, 300, 0.0, 2.0);
    emu_step(e);

    ev_push_key(e, "keyPressed", 113, "q", 0);
    emu_step(e);
    ev_push_key(e, "keyPressed", 81, "Q", 1);
    emu_step(e);

    ev_push_key(e, "keyPressed", 27, "", 0);
    emu_step(e);
    ev_push_key(e, "keyReleased", 27, "", 0);
    emu_step(e);
    long injected = e->events_in - before;
    emu_log("smoke: injected %ld synthetic events (want 50)", injected);

    run_ticks(e, 40, deadline);

    long nonzero = 0;
    for (int i = 0; i < NEET_W * NEET_H; i++)
        if (e->mainscreen.px[i]) nonzero++;
    emu_log("smoke: nonzero pixels=%ld", nonzero);
    snapshot(e);
    emu_log("smoke: state=%d ticks=%ld presents=%ld instr=%lld watchdog=%d",
            e->state, e->tick_no, e->presents, (long long)e->executed,
            e->watchdog_fired);
    if (e->watchdog_fired) {
        emu_log("smoke FAIL: watchdog tripped: %s", e->watchdog_msg);
        return 1;
    }
    if (e->state == EMU_CRASHED) {
        emu_log("smoke FAIL: crashed: %s", e->crash_msg);
        return 1;
    }
    if (injected != 50) {
        emu_log("smoke FAIL: event count != 50");
        return 1;
    }
    if (nonzero == 0) {
        emu_log("smoke FAIL: framebuffer empty (nothing rendered)");
        return 1;
    }
    emu_log("SMOKE_OK");
    return 0;
}

typedef struct {
    int ref;
    char kind[32];
    char text[48];
} Widget;

static bool call_bool_method(lua_State *L, int idx, const char *m) {
    bool ok = false;
    int top = lua_gettop(L);
    lua_getfield(L, idx < 0 ? idx - 0 : idx, m);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return false;
    }
    lua_pushvalue(L, idx < 0 ? idx - 1 : idx);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_settop(L, top);
        return false;
    }
    ok = lua_toboolean(L, -1);
    lua_settop(L, top);
    return ok;
}

static bool call_rect(lua_State *L, int idx, int *ax, int *ay, int *w, int *h) {
    int top = lua_gettop(L);
    int absidx = idx < 0 ? top + idx + 1 : idx;
    lua_getfield(L, absidx, "absRect");
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return false;
    }
    lua_pushvalue(L, absidx);
    if (lua_pcall(L, 1, 4, 0) != LUA_OK) {
        lua_settop(L, top);
        return false;
    }
    bool nums = true;
    for (int i = -4; i < 0; i++)
        if (!lua_isnumber(L, i)) nums = false;
    if (nums) {
        *ax = (int)lua_tonumber(L, -4);
        *ay = (int)lua_tonumber(L, -3);
        *w = (int)lua_tonumber(L, -2);
        *h = (int)lua_tonumber(L, -1);
    }
    lua_settop(L, top);
    return nums;
}

static void collect_widgets(lua_State *L, int idx, Widget **out, int *n,
                            int *cap) {
    int top = lua_gettop(L);
    int absidx = idx < 0 ? top + idx + 1 : idx;

    size_t len = lua_rawlen(L, absidx);
    for (size_t i = 1; i <= len; i++) {
        lua_rawgeti(L, absidx, i);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        if (*n >= *cap) {
            *cap = *cap ? *cap * 2 : 128;
            *out = (Widget *)realloc(*out, (size_t)*cap * sizeof(Widget));
        }
        Widget *wd = &(*out)[(*n)++];
        wd->ref = luaL_ref(L, LUA_REGISTRYINDEX);
        wd->kind[0] = 0;
        wd->text[0] = 0;

        lua_rawgeti(L, LUA_REGISTRYINDEX, wd->ref);
        int widx = lua_gettop(L);
        lua_getfield(L, widx, "kind");
        if (lua_isstring(L, -1)) {
            strncpy(wd->kind, lua_tostring(L, -1), sizeof(wd->kind) - 1);
            wd->kind[sizeof(wd->kind) - 1] = 0;
        }
        lua_pop(L, 1);
        lua_getfield(L, widx, "text");
        if (lua_isstring(L, -1)) {
            strncpy(wd->text, lua_tostring(L, -1), sizeof(wd->text) - 1);
            wd->text[sizeof(wd->text) - 1] = 0;
        }
        lua_pop(L, 1);
        lua_getfield(L, widx, "children");
        bool has = lua_istable(L, -1);
        if (has) collect_widgets(L, -1, out, n, cap);
        lua_pop(L, 1);
        if (!has) {
            lua_getfield(L, widx, "widgets");
            if (lua_istable(L, -1)) collect_widgets(L, -1, out, n, cap);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

static bool direct_render(Emu *e, int vp_ref, int model_ref) {
    lua_State *L = e->L;
    emu_arm_hook_for_direct(e, L);
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, vp_ref);
    lua_getfield(L, -1, "render");
    lua_rawgeti(L, LUA_REGISTRYINDEX, vp_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, model_ref);
    int st = lua_pcall(L, 2, 0, 0);
    if (st == LUA_OK) {
        lua_getfield(L, -1, "present");
        lua_rawgeti(L, LUA_REGISTRYINDEX, vp_ref);
        st = lua_pcall(L, 1, 0, 0);
    }
    bool ok = st == LUA_OK;
    if (!ok) {
        const char *msg = lua_tostring(L, -1);
        emu_log("direct render failed: %s", msg ? msg : "?");
        lua_pop(L, 1);
    }
    lua_settop(L, top);
    emu_disarm_hook(e, L);
    if (e->watchdog_fired) {
        emu_log("WATCHDOG FIRED during direct render: %s", e->watchdog_msg);
        return false;
    }
    return ok;
}

static int do_clickall(Emu *e, double deadline) {
    if (!run_ticks(e, 60, deadline)) return 1;
    if (e->state != EMU_RUN) {
        emu_log("clickall: app not running after settle");
        return 1;
    }
    lua_State *L = e->L;

    const char *hname = NULL;
    lua_getglobal(L, "NeetModeler");
    if (lua_istable(L, -1)) hname = "NeetModeler";
    else {
        lua_pop(L, 1);
        lua_getglobal(L, "NeetGuiDemo");
        if (lua_istable(L, -1)) hname = "NeetGuiDemo";
        else {
            lua_pop(L, 1);
            emu_log("clickall: no NeetModeler/NeetGuiDemo handle");
            return 1;
        }
    }

    lua_getfield(L, -1, "gui");
    int gui_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_getfield(L, -1, "vp");
    int vp_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_getfield(L, -1, "model");
    int model_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);

    Widget *ws = NULL;
    int n = 0, cap = 0;
    lua_rawgeti(L, LUA_REGISTRYINDEX, gui_ref);
    lua_getfield(L, -1, "widgets");
    if (lua_istable(L, -1)) collect_widgets(L, -1, &ws, &n, &cap);
    lua_pop(L, 2);
    emu_log("clickall: %d widgets collected (%s)", n, hname);

    int counts[7] = { 0, 0, 0, 0, 0, 0, 0 };
    int render_ok = 0, render_fail = 0;
    for (int i = 0; i < n; i++) {
        if (emu_now() > deadline) {
            emu_log("clickall: timeout");
            break;
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, ws[i].ref);
        int widx = lua_gettop(L);
        if (!lua_istable(L, widx)) {
            lua_pop(L, 1);
            counts[6]++;
            continue;
        }
        if (!call_bool_method(L, widx, "isEffectivelyVisible")) {
            lua_pop(L, 1);
            counts[6]++;
            continue;
        }
        int ax = 0, ay = 0, w = 0, h = 0;
        bool have_rect = call_rect(L, widx, &ax, &ay, &w, &h);
        lua_pop(L, 1);
        const char *kind = ws[i].kind;
        if (strcmp(kind, "button") == 0 && have_rect) {
            counts[0]++;
            int x = ax + w / 2, y = ay + h / 2;
            in_motion(e, x, y);
            run_ticks(e, 1, deadline);
            in_button(e, x, y, 0, true);
            run_ticks(e, 2, deadline);
            in_button(e, x, y, 0, false);
            run_ticks(e, 5, deadline);
        } else if (strcmp(kind, "checkbox") == 0 && have_rect) {
            counts[1]++;
            int x = ax + w / 2, y = ay + h / 2;
            in_motion(e, x, y);
            in_button(e, x, y, 0, true);
            run_ticks(e, 2, deadline);
            in_button(e, x, y, 0, false);
            run_ticks(e, 5, deadline);
        } else if (strcmp(kind, "textfield") == 0 && have_rect) {
            counts[2]++;
            int x = ax + w / 2, y = ay + h / 2;
            in_motion(e, x, y);
            in_button(e, x, y, 0, true);
            run_ticks(e, 1, deadline);
            in_button(e, x, y, 0, false);
            run_ticks(e, 2, deadline);
            ev_push_key(e, "keyPressed", 65, "a", 0);
            run_ticks(e, 2, deadline);
            ev_push_key(e, "keyPressed", 13, "", 0);
            run_ticks(e, 3, deadline);
        } else if (strcmp(kind, "dropdown") == 0 && have_rect) {
            counts[3]++;
            int x = ax + w / 2, y = ay + h / 2;
            in_motion(e, x, y);
            in_button(e, x, y, 0, true);
            run_ticks(e, 1, deadline);
            in_button(e, x, y, 0, false);
            run_ticks(e, 3, deadline);
            ev_push_key(e, "keyPressed", 27, "", 0);
            run_ticks(e, 3, deadline);
        } else if (strcmp(kind, "listbox") == 0 && have_rect) {
            counts[4]++;
            in_motion(e, ax + 5, ay + 6);
            in_button(e, ax + 5, ay + 6, 0, true);
            run_ticks(e, 1, deadline);
            in_button(e, ax + 5, ay + 6, 0, false);
            run_ticks(e, 5, deadline);
        } else if (strcmp(kind, "slider") == 0 && have_rect) {
            counts[5]++;
            int x = ax + w / 2, y = ay + h / 2;
            in_motion(e, x, y);
            in_button(e, x, y, 0, true);
            run_ticks(e, 1, deadline);
            in_motion(e, x + 20, y);
            run_ticks(e, 2, deadline);
            in_button(e, x + 20, y, 0, false);
            run_ticks(e, 4, deadline);
        } else {
            counts[6]++;
            continue;
        }
        if (e->state != EMU_RUN) {
            emu_log("clickall: app left RUN state (%d) at widget %d (%s)",
                    e->state, i, kind);
            break;
        }

        double r0 = emu_now();
        bool rok = direct_render(e, vp_ref, model_ref);
        if (rok) render_ok++;
        else {
            render_fail++;
            if (e->watchdog_fired) break;
        }
        emu_log("clickall: %d/%d kind=%s text='%.40s' rect=(%d,%d,%d,%d) "
                "ticks=%ld instr=%lldm render=%.1fs %s",
                i + 1, n, kind, ws[i].text, ax, ay, w, h, e->tick_no,
                (long long)e->executed / 1000000, emu_now() - r0,
                rok ? "ok" : "FAIL");
    }

    direct_render(e, vp_ref, model_ref);
    for (int i = 0; i < n; i++) luaL_unref(L, LUA_REGISTRYINDEX, ws[i].ref);
    free(ws);
    luaL_unref(L, LUA_REGISTRYINDEX, gui_ref);
    luaL_unref(L, LUA_REGISTRYINDEX, vp_ref);
    luaL_unref(L, LUA_REGISTRYINDEX, model_ref);
    emu_log("clickall: buttons=%d checks=%d fields=%d drops=%d lists=%d "
            "sliders=%d skipped=%d renders_ok=%d renders_fail=%d",
            counts[0], counts[1], counts[2], counts[3], counts[4], counts[5],
            counts[6], render_ok, render_fail);
    emu_log("clickall: state=%d ticks=%ld presents=%ld instr=%lld watchdog=%d",
            e->state, e->tick_no, e->presents, (long long)e->executed,
            e->watchdog_fired);
    if (e->watchdog_fired || e->state == EMU_CRASHED) return 1;
    emu_log("CLICK_ALL_DONE_NO_HANG");
    return 0;
}

static int do_watchdog_test(Emu *e, double deadline) {
    static const char spin[] = "while true do end\n";
    free(e->app_src);
    e->app_src = (char *)malloc(sizeof(spin));
    memcpy(e->app_src, spin, sizeof(spin));
    e->app_srclen = sizeof(spin) - 1;
    strcpy(e->app_chunkname, "spin");
    double t0 = emu_now();
    if (!emu_boot(e)) return 1;
    emu_log("watchdog-test: running infinite loop, watchdog=%.0fs",
            e->watchdog_secs);
    while (e->state == EMU_RUN) {
        if (emu_now() > deadline) {
            emu_log("watchdog-test FAIL: deadline without watchdog");
            return 1;
        }
        emu_step(e);
    }
    double dt = emu_now() - t0;
    if (e->watchdog_fired) {
        emu_log("WATCHDOG_FIRED after %.1fs: %s", dt, e->watchdog_msg);
        if (dt <= 20.0) {
            emu_log("WATCHDOG_OK (fired within ~15s, names the line)");
            return 0;
        }
        emu_log("watchdog too slow");
        return 1;
    }
    emu_log("watchdog-test FAIL: state=%d without watchdog: %s", e->state,
            e->crash_msg);
    return 1;
}

static int do_reboot_test(Emu *e, double deadline) {
    if (!run_ticks(e, 20, deadline)) return 1;
    if (e->state != EMU_RUN) {
        emu_log("reboot-test FAIL: not running before reboot");
        return 1;
    }
    long exec_before = e->executed;
    emu_log("reboot-test: rebooting (executed=%lld)", (long long)exec_before);
    if (!emu_boot(e)) return 1;
    if (e->executed != 0 || e->state != EMU_RUN) {
        emu_log("reboot-test FAIL: state not fresh");
        return 1;
    }
    if (!run_ticks(e, 20, deadline)) return 1;
    if (e->state != EMU_RUN) {
        emu_log("reboot-test FAIL: not running after reboot");
        return 1;
    }
    emu_log("REBOOT_OK (ticks=%ld presents=%ld)", e->tick_no, e->presents);
    return 0;
}

static void usage(const char *a0) {
    printf("neetemu %s — run NeetComputers Lua machines on your desktop\n",
           NEETEMU_VERSION);
    printf("\nusage: %s [options] [app.lua] [diskdir] [app args...]\n", a0);
    printf("\nWith no arguments, boots NeetOS on computer 0.\n");
    printf("Each computer id has its own isolated storage in\n");
    printf("  ~/.local/share/neetemu/computers/<id>/\n");
    printf("  ($XDG_DATA_HOME/neetemu/computers/<id>/ if set).\n");
    printf("Extra positionals after app/disk are passed to the app as in\n");
    printf("NeetOS `run <prog> [args...]`.\n");
    printf("\nOptions:\n");
    printf("  -c, --computer ID  boot computer ID (default 0); created\n");
    printf("                     from the bundled image on first use\n");
    printf("  --list            list computers and exit\n");
    printf("  --disk DIR        use this disk folder instead of a computer\n");
    printf("                    (advanced override, shown as 'custom')\n");
    printf("  --zoom 1|2        window scale (also Ctrl+1 / Ctrl+2)\n");
    printf("  --watchdog S      hang-detector seconds (default 10)\n");
    printf("  --version         print version and exit\n");
    printf("  -h, --help        this help\n");
    printf("\nHeadless / diagnostics:\n");
    printf("  --headless        dummy video, no pacing (for tests)\n");
    printf("  --ticks N         stop after N ticks\n");
    printf("  --timeout S       wall-clock cap (headless)\n");
    printf("  --log-tick        log every tick\n");
    printf("  --smoke           50 synthetic events incl. Q,Q quit\n");
    printf("  --clickall        click-every-widget sweep\n");
    printf("  --watchdog-test   run `while true do end`, expect watchdog\n");
    printf("  --reboot-test     boot, run, reboot, run again\n");
    printf("\n3D accelerator:\n");
    printf("  --n3d             attach 3D accelerator plugin (default lib path)\n");
    printf("  --n3d-lib PATH    attach accelerator from this libneetn3d.so\n");
    printf("  --no-n3d          detach accelerator: n3d == nil (pure-Lua fallback)\n");
    printf("                    (env NEETN3D_LIB overrides the default path)\n");
    printf("\nKeys (also listed in-app with F1):\n");
    printf("  F1 help   F9 snapshot BMP   F10 reboot   F11 fullscreen\n");
    printf("  Ctrl+1/Ctrl+2 zoom   Ctrl+L stats overlay\n");
    printf("\nExamples:\n");
    printf("  %s                        # boot NeetOS (computer 0)\n", a0);
    printf("  %s -c 3                   # boot NeetOS (computer 3)\n", a0);
    printf("  %s --list                 # show your computers\n", a0);
    printf("  %s myprog.lua             # run a Lua program on computer 0\n", a0);
    printf("  %s -c 3 myprog.lua        # ... on computer 3\n", a0);
    printf("  %s -c 3 -- prog.lua args  # app args, computer disk kept\n", a0);
    printf("  %s myprog.lua mydisk      # ... with your own disk folder\n", a0);
}

static void mkdir_p(const char *p) {
    char tmp[2048];
    strncpy(tmp, p, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    for (char *c = tmp + 1; *c; c++)
        if (*c == '/') {
            *c = 0;
            mkdir(tmp, 0755);
            *c = '/';
        }
    mkdir(tmp, 0755);
}

static bool has_bios(const char *disk) {
    char p[4096];
    snprintf(p, sizeof(p), "%s/bios/bios.lua", disk);
    return access(p, R_OK) == 0;
}

static bool copy_tree(const char *src, const char *dst) {
    DIR *d = opendir(src);
    if (!d) return false;
    mkdir_p(dst);
    bool ok = true;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char s[4096], t[4096];
        snprintf(s, sizeof(s), "%s/%s", src, de->d_name);
        snprintf(t, sizeof(t), "%s/%s", dst, de->d_name);
        struct stat st;
        if (stat(s, &st) != 0) { ok = false; break; }
        if (S_ISDIR(st.st_mode)) {
            if (!copy_tree(s, t)) { ok = false; break; }
        } else if (S_ISREG(st.st_mode)) {
            FILE *in = fopen(s, "rb");
            FILE *out = in ? fopen(t, "wb") : NULL;
            if (!in || !out) {
                if (in) fclose(in);
                if (out) fclose(out);
                ok = false;
                break;
            }
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
                if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
            fclose(in);
            fclose(out);
            if (!ok) break;
        }
    }
    closedir(d);
    return ok;
}

static void exe_dir(char *out, size_t n) {
    out[0] = 0;
#ifdef __linux__
    char link[4096];
    ssize_t len = readlink("/proc/self/exe", link, sizeof(link) - 1);
    if (len > 0) {
        link[len] = 0;
        char *sl = strrchr(link, '/');
        size_t dlen = sl ? (size_t)(sl - link + 1) : 0;
        if (dlen > 0 && dlen < n) {
            memcpy(out, link, dlen);
            out[dlen] = 0;
        }
    }
#endif
    if (!out[0]) {
        char *base = SDL_GetBasePath();
        if (base) snprintf(out, n, "%s", base);

    }
}

static void data_dir(char *out, size_t n) {
    const char *env = getenv("NEETEMU_DATA_DIR");
    if (env && *env) { snprintf(out, n, "%s", env); return; }
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) snprintf(out, n, "%s/neetemu", xdg);
    else {
        const char *home = getenv("HOME");
        if (!home) home = ".";
        snprintf(out, n, "%s/.local/share/neetemu", home);
    }
}

static void template_disk(char *out, size_t n) {
    out[0] = 0;
    char basedir[4096];
    exe_dir(basedir, sizeof(basedir));
    if (basedir[0]) {
        char cand[4096];
        snprintf(cand, sizeof(cand), "%s../share/neetemu/disk", basedir);
        if (has_bios(cand)) { snprintf(out, n, "%s", cand); return; }
        snprintf(cand, sizeof(cand), "%sdisk", basedir);
        if (has_bios(cand)) { snprintf(out, n, "%s", cand); return; }
    }
    if (has_bios("./disk")) snprintf(out, n, "./disk");
}

static bool ensure_computer(long id, char *out, size_t n) {
    char data[4096];
    data_dir(data, sizeof(data));
    snprintf(out, n, "%s/computers/%ld", data, id);
    if (has_bios(out)) return true;

    if (id == 0) {
        char legacy[4096];
        snprintf(legacy, sizeof(legacy), "%s/disk", data);
        if (has_bios(legacy)) {
            emu_log("migrating previous disk to computer 0 ('%s')", out);
            if (copy_tree(legacy, out) && has_bios(out)) return true;
            emu_log("warning: migration copy failed, "
                    "falling back to template");
        }
    }
    char tpl[4096];
    template_disk(tpl, sizeof(tpl));
    if (!tpl[0]) {
        emu_log("no template disk image found (looked next to the "
                "executable and in ./disk)");
        return false;
    }
    emu_log("creating computer %ld from template ('%s')", id, tpl);
    if (!copy_tree(tpl, out) || !has_bios(out)) {
        emu_log("failed to create computer %ld at '%s'", id, out);
        return false;
    }
    return true;
}

static void resolve_default_disk(char *out, size_t n, long computer_id) {
    if (!ensure_computer(computer_id, out, n)) {

        snprintf(out, n, "./disk");
    }
}

int main(int argc, char **argv) {
    Opts o;
    memset(&o, 0, sizeof(o));
    o.app = NULL;
    o.disk = NULL;
    o.zoom = 1;
    o.timeout = 0;
    o.with_n3d = -1;

    int pos = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) o.headless = true;
        else if (strcmp(argv[i], "--n3d") == 0) o.with_n3d = 1;
        else if (strcmp(argv[i], "--n3d-lib") == 0 && i + 1 < argc) {
            o.with_n3d = 1;
            o.n3d_lib = argv[++i];
        } else if (strcmp(argv[i], "--no-n3d") == 0) o.with_n3d = 0;
        else if (strcmp(argv[i], "--smoke") == 0) o.smoke = true;
        else if (strcmp(argv[i], "--clickall") == 0) o.clickall = true;
        else if (strcmp(argv[i], "--watchdog-test") == 0)
            o.watchdog_test = true;
        else if (strcmp(argv[i], "--reboot-test") == 0)
            o.reboot_test = true;
        else if (strcmp(argv[i], "--log-tick") == 0) {

        } else if (strcmp(argv[i], "--ticks") == 0 && i + 1 < argc)
            o.tick_cap = atol(argv[++i]);
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
            o.timeout = atof(argv[++i]);
        else if (strcmp(argv[i], "--watchdog") == 0 && i + 1 < argc) {
            i++;
        }         else if (strcmp(argv[i], "--disk") == 0 && i + 1 < argc) {
            o.disk = argv[++i];
            o.disk_given = true;
        }
        else if (strcmp(argv[i], "--computer") == 0 ||
                 strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            const char *id = argv[++i];
            char *end = NULL;
            long v = strtol(id, &end, 10);
            if (!*id || (end && *end) || v < 0 || v > 999999) {
                fprintf(stderr, "bad computer id '%s' (want 0..999999)\n",
                        id);
                return 1;
            }
            o.computer_id = v;
        } else if (strcmp(argv[i], "--list") == 0) o.list_computers = true;
        else if (strcmp(argv[i], "--zoom") == 0 && i + 1 < argc)
            o.zoom = atoi(argv[++i]);
        else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0)
            o.show_version = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--") == 0) {

            for (i++; i < argc; i++) {
                if (!o.app_given) { o.app = argv[i]; o.app_given = true; }
                else if (o.app_argc < 32) o.app_args[o.app_argc++] = argv[i];
                else { usage(argv[0]); return 1; }
            }
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 1;
        } else if (pos == 0) {
            o.app = argv[i];
            o.app_given = true;
            pos++;
        } else if (pos == 1) {
            o.disk = argv[i];
            o.disk_given = true;
            pos++;
        } else {
            if (o.app_argc < 32) o.app_args[o.app_argc++] = argv[i];
            else { usage(argv[0]); return 1; }
        }
    }

    double watchdog = 10.0;
    bool log_tick = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--watchdog") == 0 && i + 1 < argc)
            watchdog = atof(argv[++i]);
        if (strcmp(argv[i], "--log-tick") == 0) log_tick = true;
    }
    if (o.zoom != 1 && o.zoom != 2) o.zoom = 1;

    if (o.show_version) {
        printf("neetemu %s\n", NEETEMU_VERSION);
        return 0;
    }

    if (o.list_computers) {
        char data[4096];
        data_dir(data, sizeof(data));
        char dir[4096];
        snprintf(dir, sizeof(dir), "%s/computers", data);
        printf("computers in %s:\n", dir);
        DIR *d = opendir(dir);
        if (!d) {
            printf("  (none yet — computer 0 is created on first run)\n");
            return 0;
        }
        int found = 0;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            char *end = NULL;
            long v = strtol(de->d_name, &end, 10);
            if (!de->d_name[0] || (end && *end) || v < 0) continue;
            char sub[4096];
            snprintf(sub, sizeof(sub), "%s/%s/bios/bios.lua", dir,
                     de->d_name);
            printf("  %s%s\n", de->d_name,
                   access(sub, R_OK) == 0 ? "" : " (incomplete)");
            found++;
        }
        closedir(d);
        if (!found)
            printf("  (none yet — computer 0 is created on first run)\n");
        return 0;
    }

    char auto_disk[4096] = { 0 };
    char auto_app[4096] = { 0 };
    if (!o.disk_given || !o.app_given) {
        if (o.disk_given)
            snprintf(auto_disk, sizeof(auto_disk), "%s", o.disk);
        else
            resolve_default_disk(auto_disk, sizeof(auto_disk),
                                 o.computer_id);
        o.disk = auto_disk;
        if (!o.app_given) {
            snprintf(auto_app, sizeof(auto_app), "%s/bios/bios.lua",
                     auto_disk);
            o.app = auto_app;
        }
    }

    if (o.headless) {
        setenv("SDL_VIDEODRIVER", "dummy", 1);
        setenv("SDL_AUDIODRIVER", "dummy", 1);
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    g_win = SDL_CreateWindow("neetemu " NEETEMU_VERSION,
                             NEET_W * o.zoom, NEET_H * o.zoom,
                             SDL_WINDOW_RESIZABLE);
    if (!g_win) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return 1;
    }
    g_ren = SDL_CreateRenderer(g_win, NULL);
    g_tex = g_ren ? SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      NEET_W, NEET_H)
                  : NULL;
    if (g_ren) {

        SDL_SetRenderLogicalPresentation(g_ren, NEET_W, NEET_H,
                                         SDL_LOGICAL_PRESENTATION_LETTERBOX);
    }
    g_argb = (uint32_t *)malloc(NEET_W * NEET_H * 4);
    {
        const char *rname = (g_ren && g_tex) ? SDL_GetRendererName(g_ren)
                                             : "(no renderer)";
        emu_log("video driver=%s renderer=%s",
                SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver()
                                            : "?",
                rname ? rname : "?");
    }

    Emu emu;
    memset(&emu, 0, sizeof(emu));
    strncpy(emu.app_path, o.app, sizeof(emu.app_path) - 1);
    strncpy(emu.disk_root, o.disk, sizeof(emu.disk_root) - 1);
    if (o.disk_given)
        snprintf(emu.computer, sizeof(emu.computer), "custom");
    else
        snprintf(emu.computer, sizeof(emu.computer), "%ld",
                 o.computer_id);
    emu.app_argc = o.app_argc > 32 ? 32 : o.app_argc;
    for (int i = 0; i < emu.app_argc; i++) {
        strncpy(emu.app_argv[i], o.app_args[i] ? o.app_args[i] : "",
                sizeof(emu.app_argv[i]) - 1);
        emu.app_argv[i][sizeof(emu.app_argv[i]) - 1] = 0;
    }
    emu.hook_step = 1000;
    emu.tick_budget = (int64_t)3000 * 3750;
    emu.watchdog_secs = watchdog;
    emu.log_tick = log_tick;
    emu.zoom = o.zoom;
    emu.overlay = false;
    if (getenv("NEETEMU_NO_OVERLAY")) emu.overlay = false;
    if (getenv("NEETEMU_OVERLAY")) emu.overlay = true;

    if (o.with_n3d != 0) {
        const char *lib = o.n3d_lib;
        if (!lib) lib = getenv("NEETN3D_LIB");
        if (!lib) lib = "./libneetn3d.so";
        emu.n3d_handle = dlopen(lib, RTLD_NOW);
        if (!emu.n3d_handle) {
            if (o.n3d_lib) {
                emu_log("cannot load 3d plugin '%s': %s", lib, dlerror());
                return 1;
            }
            emu_log("3d accelerator: no plugin at '%s' (detached, n3d nil)", lib);
        } else {
            dlerror();
            void *sym = dlsym(emu.n3d_handle, "luaopen_n3d");
            if (!sym) {
                emu_log("3d plugin '%s' has no luaopen_n3d: %s", lib, dlerror());
                return 1;
            }
            emu.n3d_open = (lua_CFunction)sym;
            emu_log("3d accelerator: attached (%s)", lib);
        }
    } else {
        emu_log("3d accelerator: detached (n3d nil)");
    }
    emu.present = do_present;
    mkdir_p(emu.disk_root);

    double deadline = o.timeout > 0 ? emu_now() + o.timeout : 0;
    if ((o.smoke || o.clickall || o.watchdog_test || o.reboot_test) &&
        o.timeout == 0)
        deadline = emu_now() + 300;

    int rc = 0;
    if (o.watchdog_test) {
        if (!emu_boot(&emu)) return 1;
        rc = do_watchdog_test(&emu, deadline);
    } else if (o.reboot_test) {
        if (!emu_boot(&emu)) return 1;
        rc = do_reboot_test(&emu, deadline);
    } else {
        if (access(emu.app_path, R_OK) != 0) {
            emu_log("cannot open '%s'", emu.app_path);
            emu_log("hint: pass an app explicitly, e.g. "
                    "neetemu myprog.lua [diskdir]");
            if (!o.headless) {
                char msg[2048];
                snprintf(msg, sizeof(msg),
                         "Cannot open '%s'.\n\n"
                         "Run with no arguments to boot NeetOS, or pass\n"
                         "an app explicitly: neetemu myprog.lua [diskdir]",
                         emu.app_path);
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                         "NeetEmulator", msg, g_win);
            }
            return 1;
        }
        if (!emu_boot(&emu)) {
            emu_log("boot failed: %s", emu.crash_msg);
            if (!o.headless && !o.smoke && !o.clickall) {
                char msg[2048];
                snprintf(msg, sizeof(msg),
                         "Boot failed:\n%.1500s\n\n"
                         "Close the window to quit, or press F10 to retry.",
                         emu.crash_msg);
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                         "NeetEmulator", msg, g_win);
                bool live = true;
                while (live) {
                    if (!pump_input(&emu, &o)) live = false;
                    SDL_Delay(16);
                }
            }
            return 1;
        }
        if (o.smoke) {
            rc = do_smoke(&emu, deadline);
        } else if (o.clickall) {
            rc = do_clickall(&emu, deadline);
        } else if (o.headless) {
            long cap = o.tick_cap > 0 ? o.tick_cap : 1000000000L;
            for (long i = 0; i < cap; i++) {
                if (emu.state != EMU_RUN) break;
                if (deadline && emu_now() > deadline) break;
                emu_step(&emu);
                if (emu.reboot_requested) emu_boot(&emu);
            }
            emu_log("headless done: state=%d ticks=%ld presents=%ld "
                    "instr=%lld",
                    emu.state, emu.tick_no, emu.presents,
                    (long long)emu.executed);
            rc = (emu.state == EMU_CRASHED) ? 1 : 0;
        } else {

            Uint64 last_title = 0;
            bool live = true;
            Uint64 freq = SDL_GetPerformanceFrequency();
            while (live) {
                Uint64 f0 = SDL_GetPerformanceCounter();
                if (!pump_input(&emu, &o)) live = false;

                {
                    const bool *st = SDL_GetKeyboardState(NULL);
                    static bool was = false;
                    bool is = (st[SDL_SCANCODE_L] &&
                               (SDL_GetModState() & SDL_KMOD_CTRL));
                    if (is && !was) {
                        emu.overlay = !emu.overlay;
                        emu_log("overlay %s", emu.overlay ? "on" : "off");
                    }
                    was = is;
                }
                if (emu.state == EMU_RUN) {
                    emu_step(&emu);
                    if (emu.reboot_requested) emu_boot(&emu);
                }
                Uint64 nowt = SDL_GetPerformanceCounter();
                if (nowt - last_title > freq / 2) {
                    last_title = nowt;
                    char t[256];
                    const char *sn = emu.state == EMU_RUN ? "RUN" :
                                     emu.state == EMU_EXITED ? "EXITED" :
                                     emu.state == EMU_CRASHED ? "CRASHED" : "OFF";
                    snprintf(t, sizeof(t),
                             "neetemu %s [c%s %s] tick %ld instr %lldM presents %ld q %d",
                             NEETEMU_VERSION, emu.computer, sn, emu.tick_no,
                             (long long)emu.executed / 1000000,
                             emu.presents, ev_queued_total(&emu));
                    SDL_SetWindowTitle(g_win, t);
                }

                Uint64 f1 = SDL_GetPerformanceCounter();
                double ms = (double)(f1 - f0) * 1000.0 / (double)freq;
                if (ms < 16.0) SDL_Delay((Uint32)(16.0 - ms));

                static long last_sum = 0;
                if (emu.tick_no - last_sum >= 600) {
                    last_sum = emu.tick_no;
                    emu_log("tick %ld instr %lld presents %ld queued %d "
                            "api_errors %ld",
                            emu.tick_no, (long long)emu.executed,
                            emu.presents, ev_queued_total(&emu),
                            emu.api_errors);
                }
            }
        }
    }

    emu_close_lua(&emu);
    free(emu.app_src);
    free(g_argb);
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    SDL_Quit();
    return rc;
}
