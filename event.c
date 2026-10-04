#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

#include "lua.h"
#include "lauxlib.h"
#include "emu.h"

static const char *label_names[NEET_NLABELS] = {
    "UNLABELED", "USER", "SYSTEM", "NETWORK", "PERIPHERAL", "COMPATIBILITY"
};

int event_label_index(const char *s) {
    if (!s) return -1;
    for (int i = 0; i < NEET_NLABELS; i++)
        if (strcasecmp(s, label_names[i]) == 0) return i;
    return -1;
}

void ev_free(Event *ev) {
    for (int i = 0; i < ev->nargs; i++)
        if (ev->tag[i] == EV_STR) free(ev->sval[i]);
}

bool ev_enqueue(Emu *e, int label, const Event *src) {
    if (label < 0 || label >= NEET_NLABELS) return false;
    EQueue *q = &e->queues[label];
    if (!q->ev) q->ev = (Event *)calloc(NEET_MAXQ, sizeof(Event));
    if (!q->ev) return false;
    if (q->len == NEET_MAXQ) {
        ev_free(&q->ev[q->head]);
        q->head = (q->head + 1) % NEET_MAXQ;
        q->len--;
    }
    Event *dst = &q->ev[(q->head + q->len) % NEET_MAXQ];
    memset(dst, 0, sizeof(*dst));
    snprintf(dst->name, sizeof(dst->name), "%s", src->name);
    dst->nargs = src->nargs > 4 ? 4 : src->nargs;
    for (int i = 0; i < dst->nargs; i++) {
        dst->tag[i] = src->tag[i];
        dst->ival[i] = src->ival[i];
        dst->dval[i] = src->dval[i];
        dst->slen[i] = src->slen[i];
        if (src->tag[i] == EV_STR && src->sval[i]) {
            dst->sval[i] = (char *)malloc(src->slen[i] ? src->slen[i] : 1);
            if (src->slen[i]) memcpy(dst->sval[i], src->sval[i], src->slen[i]);
        }
    }
    q->len++;
    e->events_in++;
    return true;
}

static void ev_note(Emu *e, const Event *v) {
    char tmp[256];
    int n = snprintf(tmp, sizeof(tmp), "%s", v->name);
    for (int i = 0; i < v->nargs && n < (int)sizeof(tmp) - 24; i++) {
        if (v->tag[i] == EV_INT) n += snprintf(tmp + n, sizeof(tmp) - n, " %lld", (long long)v->ival[i]);
        else if (v->tag[i] == EV_DBL) n += snprintf(tmp + n, sizeof(tmp) - n, " %.3f", v->dval[i]);
        else if (v->tag[i] == EV_STR) n += snprintf(tmp + n, sizeof(tmp) - n, " '%.*s'", (int)(v->slen[i] > 12 ? 12 : v->slen[i]), v->sval[i]);
    }
    strncpy(e->last_event, tmp, sizeof(e->last_event) - 1);
    e->last_event[sizeof(e->last_event) - 1] = 0;
}

void ev_push_mouse(Emu *e, const char *name, int x, int y, int key) {
    Event v;
    memset(&v, 0, sizeof(v));
    strncpy(v.name, name, sizeof(v.name) - 1);
    v.nargs = 3;
    v.tag[0] = EV_INT; v.ival[0] = x;
    v.tag[1] = EV_INT; v.ival[1] = y;
    v.tag[2] = EV_INT; v.ival[2] = key;
    ev_enqueue(e, 1 , &v);
    ev_note(e, &v);
}

void ev_push_move(Emu *e, int x, int y) {
    Event v;
    memset(&v, 0, sizeof(v));
    strcpy(v.name, "mouseMoved");
    v.nargs = 2;
    v.tag[0] = EV_INT; v.ival[0] = x;
    v.tag[1] = EV_INT; v.ival[1] = y;
    ev_enqueue(e, 1, &v);
    ev_note(e, &v);
}

void ev_push_scroll(Emu *e, int x, int y, double h, double v) {
    Event ev;
    memset(&ev, 0, sizeof(ev));
    strcpy(ev.name, "mouseScrolled");
    ev.nargs = 4;
    ev.tag[0] = EV_INT; ev.ival[0] = x;
    ev.tag[1] = EV_INT; ev.ival[1] = y;
    ev.tag[2] = EV_DBL; ev.dval[2] = h;
    ev.tag[3] = EV_DBL; ev.dval[3] = v;
    ev_enqueue(e, 1, &ev);
    ev_note(e, &ev);
}

void ev_push_key(Emu *e, const char *name, int code, const char *letter, int mods) {
    Event v;
    memset(&v, 0, sizeof(v));
    strncpy(v.name, name, sizeof(v.name) - 1);
    v.nargs = 3;
    v.tag[0] = EV_INT; v.ival[0] = code;
    v.tag[1] = EV_STR;
    v.sval[1] = (char *)letter;
    v.slen[1] = strlen(letter);
    v.tag[2] = EV_INT; v.ival[2] = mods;
    ev_enqueue(e, 1, &v);
    ev_note(e, &v);
}

int ev_queued_total(Emu *e) {
    int n = 0;
    for (int i = 0; i < NEET_NLABELS; i++) n += e->queues[i].len;
    return n;
}

static void push_value(lua_State *L, const Event *ev, int i) {
    switch (ev->tag[i]) {
    case EV_INT: lua_pushinteger(L, (lua_Integer)ev->ival[i]); break;
    case EV_DBL: lua_pushnumber(L, ev->dval[i]); break;
    case EV_STR: lua_pushlstring(L, ev->sval[i], ev->slen[i]); break;
    case EV_BOOL: lua_pushboolean(L, (int)ev->ival[i]); break;
    default: lua_pushnil(L); break;
    }
}

static void push_event(lua_State *L, const Event *ev) {
    lua_newtable(L);
    lua_pushstring(L, ev->name);
    lua_rawseti(L, -2, 1);
    for (int i = 0; i < ev->nargs; i++) {
        push_value(L, ev, i);
        lua_rawseti(L, -2, 2 + i);
    }
}

static int l_queueEvent(lua_State *L) {
    Emu *e = emu_from(L);
    const char *cat = lua_tostring(L, 1);
    const char *name = lua_tostring(L, 2);
    if (!cat || !name) luaL_error(L, "queueEvent needs category + name");
    int label;
    if (strcasecmp(cat, "all") == 0) label = 0;
    else {
        label = event_label_index(cat);
        if (label < 0) luaL_error(L, "Invalid event category '%s'", cat);
    }
    Event v;
    memset(&v, 0, sizeof(v));
    strncpy(v.name, name, sizeof(v.name) - 1);
    int top = lua_gettop(L);
    v.nargs = top - 2 > 4 ? 4 : (top - 2 < 0 ? 0 : top - 2);

    const char *tmp[4];
    size_t tmplen[4];
    for (int i = 0; i < v.nargs; i++) {
        int ai = 3 + i;
        if (lua_isinteger(L, ai)) { v.tag[i] = EV_INT; v.ival[i] = (int64_t)lua_tointeger(L, ai); }
        else if (lua_isnumber(L, ai)) { v.tag[i] = EV_DBL; v.dval[i] = lua_tonumber(L, ai); }
        else if (lua_isstring(L, ai)) {
            v.tag[i] = EV_STR;
            tmp[i] = lua_tolstring(L, ai, &tmplen[i]);
            v.sval[i] = (char *)tmp[i];
            v.slen[i] = tmplen[i];
        } else if (lua_isboolean(L, ai)) { v.tag[i] = EV_BOOL; v.ival[i] = lua_toboolean(L, ai); }
        else v.tag[i] = EV_NIL;
    }
    ev_enqueue(e, label, &v);
    ev_note(e, &v);
    return 0;
}

static int l_getQueue(lua_State *L) {
    Emu *e = emu_from(L);
    const char *cat = lua_tostring(L, 1);
    const char *filter = NULL;
    if (!lua_isnoneornil(L, 2)) filter = lua_tostring(L, 2);
    int label = event_label_index(cat ? cat : "");
    if (label < 0) luaL_error(L, "Invalid event category '%s'", cat ? cat : "nil");
    EQueue *q = &e->queues[label];
    lua_newtable(L);
    int out = 0;
    if (q->ev) {

        int keep = 0;
        for (int i = 0; i < q->len; i++) {
            Event *ev = &q->ev[(q->head + i) % NEET_MAXQ];
            if (filter && strcmp(ev->name, filter) != 0) {
                if (keep != i) q->ev[(q->head + keep) % NEET_MAXQ] = q->ev[(q->head + i) % NEET_MAXQ];
                keep++;
                continue;
            }
            push_event(L, ev);
            lua_rawseti(L, -2, ++out);
            e->events_out++;
            ev_free(ev);
        }
        if (!filter) {
            q->head = 0;
            q->len = 0;
            memset(q->ev, 0, NEET_MAXQ * sizeof(Event));
        } else {

            int old = q->len;
            for (int j = keep; j < old; j++)
                memset(&q->ev[(q->head + j) % NEET_MAXQ], 0,
                       sizeof(Event));
            q->len = keep;
        }
    }
    return 1;
}

static int l_getFirst(lua_State *L) {
    Emu *e = emu_from(L);
    const char *cat = lua_tostring(L, 1);
    const char *filter = NULL;
    if (!lua_isnoneornil(L, 2)) filter = lua_tostring(L, 2);
    int label = event_label_index(cat ? cat : "");
    if (label < 0) luaL_error(L, "Invalid event category '%s'", cat ? cat : "nil");
    EQueue *q = &e->queues[label];
    if (q->ev) {
        for (int i = 0; i < q->len; i++) {
            int at = (q->head + i) % NEET_MAXQ;
            if (filter && strcmp(q->ev[at].name, filter) != 0) continue;
            push_event(L, &q->ev[at]);
            ev_free(&q->ev[at]);

            for (int j = i; j < q->len - 1; j++)
                q->ev[(q->head + j) % NEET_MAXQ] = q->ev[(q->head + j + 1) % NEET_MAXQ];
            q->len--;
            memset(&q->ev[(q->head + q->len) % NEET_MAXQ], 0, sizeof(Event));
            e->events_out++;
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

static int l_clear(lua_State *L) {
    Emu *e = emu_from(L);
    if (lua_isnoneornil(L, 1)) {
        for (int i = 0; i < NEET_NLABELS; i++) {
            EQueue *q = &e->queues[i];
            if (q->ev) {
                for (int j = 0; j < q->len; j++)
                    ev_free(&q->ev[(q->head + j) % NEET_MAXQ]);
                q->head = q->len = 0;
                memset(q->ev, 0, NEET_MAXQ * sizeof(Event));
            }
        }
        return 0;
    }
    const char *cat = lua_tostring(L, 1);
    int label = event_label_index(cat ? cat : "");
    if (label < 0) luaL_error(L, "Invalid event category '%s'", cat ? cat : "nil");
    EQueue *q = &e->queues[label];
    if (q->ev) {
        for (int j = 0; j < q->len; j++)
            ev_free(&q->ev[(q->head + j) % NEET_MAXQ]);
        q->head = q->len = 0;
        memset(q->ev, 0, NEET_MAXQ * sizeof(Event));
    }
    return 0;
}

void event_register(Emu *e) {
    static const luaL_Reg fns[] = {
        { "queueEvent", l_queueEvent },
        { "getQueue", l_getQueue },
        { "getFirst", l_getFirst },
        { "clear", l_clear },
        { NULL, NULL },
    };
    lua_newtable(e->L);
    luaL_setfuncs(e->L, fns, 0);
    lua_setglobal(e->L, "event");
}
