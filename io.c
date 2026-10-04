#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "emu.h"

static int io_getPeripherals(lua_State *L) {
    lua_newtable(L);
    return 1;
}

static int io_getType(lua_State *L) {
    if (lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "UUID invalidly formatted");
    return luaL_error(L, "Peripheral not found");
}

static int io_getTag(lua_State *L) {
    if (lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "UUID invalidly formatted");
    return luaL_error(L, "Peripheral not found");
}

static int io_setTag(lua_State *L) {
    if (lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "UUID invalidly formatted");
    return luaL_error(L, "Peripheral not found");
}

static int io_isCompatibility(lua_State *L) {
    if (lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "UUID invalidly formatted");
    return luaL_error(L, "Peripheral not found");
}

static int io_queryTag(lua_State *L) {
    if (lua_type(L, 1) != LUA_TSTRING || lua_tostring(L, 1)[0] == 0)
        return luaL_error(L, "tag cant be blank");
    const char *t = lua_tostring(L, 1);
    while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r') t++;
    if (*t == 0)
        return luaL_error(L, "tag cant be blank");
    lua_newtable(L);
    return 1;
}

static int io_queryType(lua_State *L) {
    lua_newtable(L);
    return 1;
}

static int io_wrapPeripheral(lua_State *L) {
    if (lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "UUID invalidly formatted");
    return luaL_error(L, "Peripheral not found");
}

static int io_callFunction(lua_State *L) {
    if (lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "UUID invalidly formatted");
    return luaL_error(L, "Peripheral not found");
}

static int io_isViewed(lua_State *L) {
    lua_pushboolean(L, 0);
    return 1;
}

static int io_broadcastLocal(lua_State *L) {
    (void)L;
    return 0;
}

void io_register(Emu *e) {
    static const luaL_Reg fns[] = {
        { "getPeripherals", io_getPeripherals },
        { "getType", io_getType },
        { "getTag", io_getTag },
        { "setTag", io_setTag },
        { "isCompatibility", io_isCompatibility },
        { "queryTag", io_queryTag },
        { "queryType", io_queryType },
        { "wrapPeripheral", io_wrapPeripheral },
        { "callFunction", io_callFunction },
        { "isViewed", io_isViewed },
        { "broadcastLocal", io_broadcastLocal },
        { NULL, NULL },
    };
    lua_newtable(e->L);
    luaL_setfuncs(e->L, fns, 0);
    lua_setglobal(e->L, "io");
}
