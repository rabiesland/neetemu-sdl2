#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "emu.h"

double emu_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void emu_log(const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    printf("[neetemu] %s\n", tmp);
    fflush(stdout);
}

void emu_api_note(Emu *e, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->last_api, sizeof(e->last_api), fmt, ap);
    va_end(ap);
}

void emu_event_note(Emu *e, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->last_event, sizeof(e->last_event), fmt, ap);
    va_end(ap);
}

Emu *emu_from(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "neetemu");
    Emu *e = (Emu *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return e;
}

static void count_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    lua_getfield(L, LUA_REGISTRYINDEX, "neetemu");
    Emu *e = (Emu *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!e) return;
    e->executed += e->hook_step;
    e->tick_instr += e->hook_step;

    double now = emu_now();
    if (now - e->last_progress_wall > e->watchdog_secs) {

        luaL_where(L, 0);
        const char *where = lua_tostring(L, -1);
        char ctx[1500];
        snprintf(ctx, sizeof(ctx),
                 "watchdog: no app yield for %.1fs — stuck in Lua loop "
                 "(NOT blocked in host call) at %s "
                 "[tick %ld, instr-this-tick %lld, executed %lld] "
                 "[last API: %s] [last event: %s]",
                 now - e->last_progress_wall,
                 where ? where : "?",
                 e->tick_no, (long long)e->tick_instr,
                 (long long)e->executed,
                 e->last_api[0] ? e->last_api : "-",
                 e->last_event[0] ? e->last_event : "-");
        lua_pop(L, 1);
        e->watchdog_fired = true;
        strncpy(e->watchdog_msg, ctx, sizeof(e->watchdog_msg) - 1);
        luaL_error(L, "%s", ctx);
        return;
    }

    if (!e->no_budget_yield && e->tick_instr >= e->tick_budget) {
        e->budget_yield = true;
        lua_markforcedyield(L);
        lua_yield(L, 0);
    }
}

static int emu_print(lua_State *L) {
    Emu *e = emu_from(L);
    int n = lua_gettop(L);
    char buf[4096];
    size_t pos = 0;
    for (int i = 1; i <= n && pos + 1 < sizeof(buf); i++) {
        size_t len = 0;
        const char *s = luaL_tolstring(L, i, &len);
        if (i > 1 && pos + 4 < sizeof(buf)) {
            memcpy(buf + pos, "    ", 4);
            pos += 4;
        }
        size_t cp = len;
        if (cp > sizeof(buf) - pos - 1) cp = sizeof(buf) - pos - 1;
        memcpy(buf + pos, s, cp);
        pos += cp;
        lua_pop(L, 1);
    }
    buf[pos] = 0;
    emu_log("lua: %s", buf);
    if (e) {
        char tmp[200];
        snprintf(tmp, sizeof(tmp), "print: %.180s", buf);
        emu_api_note(e, "%s", tmp);
    }
    return 0;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *b = (char *)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return NULL; }
    size_t r = fread(b, 1, (size_t)sz, f);
    fclose(f);
    b[r] = 0;
    if (len) *len = r;
    return b;
}

bool emu_boot(Emu *e) {
    emu_close_lua(e);
    e->L = luaL_newstate();
    if (!e->L) return false;

    luaL_openlibs(e->L);
    if (e->n3d_open) {
        luaL_requiref(e->L, "n3d", e->n3d_open, 1);
        lua_pop(e->L, 1);
    }

    lua_pushlightuserdata(e->L, e);
    lua_setfield(e->L, LUA_REGISTRYINDEX, "neetemu");

    const char *kill[] = { "os", "dofile", "loadfile", "collectgarbage",
                           "package", "require", "warn", NULL };
    for (int i = 0; kill[i]; i++) {
        lua_pushnil(e->L);
        lua_setglobal(e->L, kill[i]);
    }
    lua_pushcfunction(e->L, emu_print);
    lua_setglobal(e->L, "print");

    screen_register(e);
    event_register(e);
    files_register(e);
    chip_register(e);
    io_register(e);

    for (int i = 0; i < NEET_NLABELS; i++) {
        EQueue *q = &e->queues[i];
        if (q->ev) {
            for (int j = 0; j < q->len; j++)
                ev_free(&q->ev[(q->head + j) % NEET_MAXQ]);
            q->head = q->len = 0;
        }
    }

    e->co = lua_newthread(e->L);
    e->co_ref = luaL_ref(e->L, LUA_REGISTRYINDEX);
    lua_sethook(e->co, count_hook, LUA_MASKCOUNT, e->hook_step);

    int st;
    if (e->app_src) {
        st = luaL_loadbuffer(e->co, e->app_src, e->app_srclen,
                             e->app_chunkname[0] ? e->app_chunkname : "app");
    } else {
        size_t len = 0;
        char *src = read_file(e->app_path, &len);
        if (!src) {
            emu_log("cannot read app file '%s'", e->app_path);
            snprintf(e->crash_msg, sizeof(e->crash_msg),
                     "cannot read app file '%s'", e->app_path);
            e->state = EMU_CRASHED;
            e->has_app = false;
            return false;
        }
        const char *base = strrchr(e->app_path, '/');
        base = base ? base + 1 : e->app_path;
        st = luaL_loadbuffer(e->co, src, len, base);
        free(src);
    }
    if (st != LUA_OK) {
        const char *msg = lua_tostring(e->co, -1);
        emu_log("app compile error: %s", msg ? msg : "?");
        snprintf(e->crash_msg, sizeof(e->crash_msg), "compile error: %s",
                 msg ? msg : "?");
        e->state = EMU_CRASHED;
        e->has_app = false;
        return false;
    }

    if (e->app_argc > 32) e->app_argc = 32;
    if (e->app_argc < 0) e->app_argc = 0;
    for (int i = 0; i < e->app_argc; i++)
        lua_pushstring(e->co, e->app_argv[i]);
    e->app_first_resume = true;
    lua_newtable(e->L);
    for (int i = 0; i < e->app_argc; i++) {
        lua_pushstring(e->L, e->app_argv[i]);
        lua_rawseti(e->L, -2, i + 1);
    }
    lua_setglobal(e->L, "arg");
    e->has_app = true;
    e->state = EMU_RUN;
    e->executed = 0;
    e->watchdog_fired = false;
    e->watchdog_msg[0] = 0;
    e->crash_msg[0] = 0;
    e->last_api[0] = 0;
    e->last_event[0] = 0;
    e->reboot_requested = false;
    e->last_progress_wall = emu_now();

    e->mouse_x = e->mouse_y = -1;
    e->last_moved_x = e->last_moved_y = -1;
    for (int i = 0; i < 8; i++)
        e->btn[i].held = e->btn[i].exited = false;
    emu_log("booted '%s' (disk root '%s')",
            e->app_src ? e->app_chunkname : e->app_path, e->disk_root);
    return true;
}

void emu_close_lua(Emu *e) {
    if (e->L) {
        lua_close(e->L);
        e->L = NULL;
        e->co = NULL;
        e->has_app = false;
    }
    free(e->mainscreen.px);
    e->mainscreen.px = NULL;

    for (int i = 0; i < NEET_NLABELS; i++) {
        EQueue *q = &e->queues[i];
        if (q->ev) {
            for (int j = 0; j < q->len; j++)
                ev_free(&q->ev[(q->head + j) % NEET_MAXQ]);
            free(q->ev);
            q->ev = NULL;
            q->head = q->len = 0;
        }
    }
}

void emu_step(Emu *e) {
    if (e->state != EMU_RUN || !e->has_app) return;
    e->tick_no++;
    e->tick_instr = 0;
    e->resumes_this_tick = 0;

    lua_gc(e->L, LUA_GCSTEP, 0);
    double t0 = emu_now();
    for (;;) {
        int nres = 0;
        int nargs = 0;
        if (e->app_first_resume) {
            nargs = e->app_argc;
            e->app_first_resume = false;
        }
        int st = lua_resume(e->co, e->L, nargs, &nres);
        e->resumes_this_tick++;
        if (st == LUA_YIELD || st == LUA_OK) {

            for (int i = 0; i < nres && lua_gettop(e->co) > 0; i++)
                lua_pop(e->co, 1);
            nres = 0;
        }
        if (st == LUA_YIELD) {
            if (e->budget_yield) {

                e->budget_yield = false;
                if (e->log_tick)
                    emu_log("tick %ld: budget suspend after %lld instr",
                            e->tick_no, (long long)e->tick_instr);
                return;
            }

            e->frames_drawn++;
            e->last_progress_wall = emu_now();
            if (e->log_tick)
                emu_log("tick %ld: app yield, %lld instr, %.2fms",
                        e->tick_no, (long long)e->tick_instr,
                        (emu_now() - t0) * 1000.0);
            return;
        }
        if (st == LUA_OK) {
            e->state = EMU_EXITED;
            emu_log("app returned (tick %ld) — exited, last frame kept. "
                    "F10 to reboot.", e->tick_no);
            return;
        }

        const char *msg = lua_tostring(e->co, -1);
        char trace[4096];
        luaL_traceback(e->L, e->co, msg ? msg : "?", 0);
        const char *tb = lua_tostring(e->L, -1);
        snprintf(trace, sizeof(trace), "%s", tb ? tb : (msg ? msg : "?"));
        lua_pop(e->L, 1);
        if (e->watchdog_fired) {
            emu_log("WATCHDOG FIRED: %s", e->watchdog_msg);
            emu_log("traceback: %s", trace);
            snprintf(e->crash_msg, sizeof(e->crash_msg), "%.1800s",
                     e->watchdog_msg);
        } else {
            emu_log("app error (tick %ld): %s", e->tick_no, trace);
            snprintf(e->crash_msg, sizeof(e->crash_msg), "%.1800s", trace);
        }
        e->state = EMU_CRASHED;
        e->api_errors++;
        emu_api_note(e, "top-level error");
        return;
    }
}

void emu_arm_hook_for_direct(Emu *e, lua_State *L) {
    e->no_budget_yield = true;
    lua_sethook(L, count_hook, LUA_MASKCOUNT, 1000);
}

void emu_disarm_hook(Emu *e, lua_State *L) {
    e->no_budget_yield = false;
    lua_sethook(L, NULL, 0, 0);
}
