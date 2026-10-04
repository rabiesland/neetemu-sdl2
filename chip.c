#include <string.h>
#include <time.h>

#include "compat.h"
#include "lua.h"
#include "lauxlib.h"
#include "emu.h"

static int c_getTime(lua_State *L) {
    Emu *e = emu_from(L);
    lua_pushnumber(L, (lua_Number)e->executed / 1000.0);
    return 1;
}

static int c_getUnixTime(lua_State *L) {
    struct timespec ts;
    clockGetTime(clockRealtime, &ts);
    lua_pushnumber(L, (lua_Number)ts.tv_sec + (lua_Number)ts.tv_nsec / 1e9);
    return 1;
}

static int c_getLunarTime(lua_State *L) {
    lua_pushinteger(L, 0);
    return 1;
}

static int c_getUUID(lua_State *L) {
    lua_pushstring(L, "emulator-uuid-0000");
    return 1;
}

static int c_getMachine(lua_State *L) {
    lua_pushstring(L, "neetemu");
    return 1;
}

static int c_shutdown(lua_State *L) {
    Emu *e = emu_from(L);
    e->state = EMU_OFF;
    emu_log("chip.shutdown() called");
    return 0;
}

static int c_reboot(lua_State *L) {
    Emu *e = emu_from(L);
    e->reboot_requested = true;
    emu_log("chip.reboot() called");
    return 0;
}

static int c_crash(lua_State *L) {
    Emu *e = emu_from(L);
    const char *msg = lua_tostring(L, 1);
    snprintf(e->crash_msg, sizeof(e->crash_msg), "%s",
             msg ? msg : "crashed");
    e->state = EMU_CRASHED;
    emu_log("chip.crash(%s)", e->crash_msg);
    return 0;
}

static int c_version(lua_State *L) {
    lua_pushstring(L, "emulator");
    return 1;
}

void chip_register(Emu *e) {
    static const luaL_Reg fns[] = {
        { "getTime", c_getTime },
        { "getUnixTime", c_getUnixTime },
        { "getLunarTime", c_getLunarTime },
        { "getUUID", c_getUUID },
        { "getMachine", c_getMachine },
        { "shutdown", c_shutdown },
        { "reboot", c_reboot },
        { "crash", c_crash },
        { "version", c_version },
        { NULL, NULL },
    };
    lua_newtable(e->L);
    luaL_setfuncs(e->L, fns, 0);
    lua_setglobal(e->L, "chip");
}
