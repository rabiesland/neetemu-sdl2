#ifndef NEETEMU_H
#define NEETEMU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "lua.h"

#define NEET_W 800
#define NEET_H 600
#define NEET_MAXQ 75
#define NEET_NLABELS 6

typedef enum { EMU_RUN, EMU_EXITED, EMU_CRASHED, EMU_OFF } EmuState;

typedef struct Screen Screen;
struct Screen {
    int w, h;
    uint32_t *px;
    bool drawable;
    bool owned;
};

#define EV_INT 0
#define EV_DBL 1
#define EV_STR 2
#define EV_BOOL 3
#define EV_NIL 4

typedef struct Event Event;
struct Event {
    char name[48];
    int nargs;
    int tag[4];
    int64_t ival[4];
    double dval[4];
    char *sval[4];
    size_t slen[4];
};

typedef struct EQueue EQueue;
struct EQueue {
    Event *ev;
    int head, len;
};

typedef struct Emu Emu;
struct Emu {
    lua_State *L;
    lua_State *co;
    int co_ref;
    bool has_app;

    Screen mainscreen;

    EQueue queues[NEET_NLABELS];

    int64_t executed;
    int64_t tick_instr;
    int64_t tick_budget;
    int hook_step;
    bool budget_yield;
    bool no_budget_yield;
    int resumes_this_tick;
    double watchdog_secs;
    double last_progress_wall;
    bool watchdog_fired;
    char watchdog_msg[2048];

    EmuState state;
    char crash_msg[2048];
    bool reboot_requested;

    long tick_no;
    long presents;
    long events_in, events_out;
    long api_errors;
    long frames_drawn;

    int mouse_x, mouse_y;
    int last_moved_x, last_moved_y;
    struct { bool held; bool exited; int x, y; } btn[8];

    char last_api[256];
    char last_event[256];
    bool log_tick;
    int zoom;
    bool overlay;
    bool show_help;

    lua_CFunction n3d_open;
    void *n3d_handle;

    char disk_root[1024];
    char computer[64];

    char app_path[1024];
    char *app_src;
    size_t app_srclen;
    char app_chunkname[256];

    int app_argc;
    char app_argv[32][256];
    bool app_first_resume;

    void (*present)(Emu *e);
    bool present_failed;
};

double emu_now(void);

Emu *emu_from(lua_State *L);

void emu_log(const char *fmt, ...);
void emu_api_note(Emu *e, const char *fmt, ...);
void emu_event_note(Emu *e, const char *fmt, ...);

bool emu_boot(Emu *e);
void emu_close_lua(Emu *e);
void emu_step(Emu *e);
void emu_arm_hook_for_direct(Emu *e, lua_State *L);
void emu_disarm_hook(Emu *e, lua_State *L);

void screen_register(Emu *e);
void screen_push_table(lua_State *L, Emu *e, Screen *s);
int32_t nee_jrshift(int32_t v, int s);
uint32_t nee_pack(int r, int g, int b, int a);
uint32_t nee_blend(uint32_t rgb, uint32_t rgba);

int event_label_index(const char *s);
void event_register(Emu *e);
void ev_free(Event *ev);
bool ev_enqueue(Emu *e, int label, const Event *src);
void ev_push_mouse(Emu *e, const char *name, int x, int y, int key);
void ev_push_move(Emu *e, int x, int y);
void ev_push_scroll(Emu *e, int x, int y, double h, double v);
void ev_push_key(Emu *e, const char *name, int code, const char *letter, int mods);
int ev_queued_total(Emu *e);

void files_register(Emu *e);

void chip_register(Emu *e);

void io_register(Emu *e);

void overlay_compose(const Emu *e, uint32_t *argb);
void overlay_help(const Emu *e, uint32_t *argb);
void overlay_crash_text(const Emu *e, char *out, size_t n);

#endif
