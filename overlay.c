#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "emu.h"

typedef struct { char ch; unsigned char r[7]; } Glyph;

static const Glyph font[] = {
    { ' ', { 0, 0, 0, 0, 0, 0, 0 } },
    { '!', { 4, 4, 4, 4, 4, 0, 4 } },
    { '\'', { 0x0C, 0x0C, 4, 0, 0, 0, 0 } },
    { '(', { 8, 4, 2, 2, 2, 4, 8 } },
    { ')', { 2, 4, 8, 8, 8, 4, 2 } },
    { '*', { 0, 0x0A, 4, 0x1F, 4, 0x0A, 0 } },
    { '+', { 0, 4, 4, 0x1F, 4, 4, 0 } },
    { ',', { 0, 0, 0, 0, 0x0C, 4, 8 } },
    { '-', { 0, 0, 0, 0x1F, 0, 0, 0 } },
    { '.', { 0, 0, 0, 0, 0, 0x0C, 0x0C } },
    { '/', { 0x10, 0x10, 8, 8, 4, 4, 2 } },
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
    { '1', { 4, 0x0C, 4, 4, 4, 4, 0x0E } },
    { '2', { 0x0E, 0x11, 0x10, 0x0C, 6, 1, 0x1F } },
    { '3', { 0x1F, 8, 4, 2, 0x10, 0x11, 0x0E } },
    { '4', { 2, 6, 0x0A, 0x12, 0x1F, 2, 2 } },
    { '5', { 0x1F, 1, 0x0F, 0x10, 0x10, 0x11, 0x0E } },
    { '6', { 0x0C, 2, 1, 0x0F, 0x11, 0x11, 0x0E } },
    { '7', { 0x1F, 0x10, 8, 4, 2, 2, 2 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
    { '9', { 0x0E, 0x11, 0x11, 0x1E, 0x10, 8, 6 } },
    { ':', { 0, 0x0C, 0x0C, 0, 0x0C, 0x0C, 0 } },
    { '=', { 0, 0, 0x1F, 0, 0x1F, 0, 0 } },
    { '?', { 0x0E, 0x11, 0x10, 4, 4, 0, 4 } },
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 1, 1, 1, 0x11, 0x0E } },
    { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
    { 'E', { 0x1F, 1, 1, 0x0E, 1, 1, 0x1F } },
    { 'F', { 0x1F, 1, 1, 0x0E, 1, 1, 1 } },
    { 'G', { 0x0E, 0x11, 1, 0x19, 0x11, 0x11, 0x0E } },
    { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0E, 4, 4, 4, 4, 4, 0x0E } },
    { 'J', { 7, 2, 2, 2, 2, 0x12, 0x0C } },
    { 'K', { 0x11, 0x12, 0x0C, 8, 0x0C, 0x12, 0x11 } },
    { 'L', { 1, 1, 1, 1, 1, 1, 0x1F } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 1, 1, 1 } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
    { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x0C, 0x12, 0x11 } },
    { 'S', { 0x0E, 0x11, 1, 0x0E, 0x10, 0x11, 0x0E } },
    { 'T', { 0x1F, 4, 4, 4, 4, 4, 4 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 4 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 } },
    { 'X', { 0x11, 0x11, 0x0A, 4, 0x0A, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x0A, 4, 4, 4, 4 } },
    { 'Z', { 0x1F, 0x10, 8, 4, 2, 1, 0x1F } },
    { '_', { 0, 0, 0, 0, 0, 0, 0x1F } },
    { '%', { 0x19, 0x1A, 2, 4, 8, 0x15, 0x13 } },
};

static const unsigned char *glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); i++)
        if (font[i].ch == c) return font[i].r;
    for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); i++)
        if (font[i].ch == '?') return font[i].r;
    return font[0].r;
}

static void draw_text(uint32_t *argb, int x0, int y0, const char *s,
                      uint32_t color) {
    int x = x0;
    for (const char *p = s; *p; p++, x += 6) {
        const unsigned char *g = glyph(*p);
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g[r] & (1u << c)) {
                    int xx = x + c, yy = y0 + r;
                    if (xx >= 0 && xx < NEET_W && yy >= 0 && yy < NEET_H)
                        argb[xx + yy * NEET_W] = color;
                }
    }
}

static const char *state_name(EmuState s) {
    switch (s) {
    case EMU_RUN: return "RUN";
    case EMU_EXITED: return "EXITED";
    case EMU_CRASHED: return "CRASHED";
    case EMU_OFF: return "OFF";
    }
    return "?";
}

void overlay_compose(const Emu *e, uint32_t *argb) {

    for (int y = 0; y < 9; y++)
        for (int x = 0; x < NEET_W; x++)
            argb[x + y * NEET_W] = 0xFF101018u;
    char line[160];
    const char *st = state_name(e->state);
    if (e->state == EMU_RUN) {
        snprintf(line, sizeof(line),
                 "TICK %ld INSTR %lld PRES %ld Q %d %.64s",
                 e->tick_no, (long long)e->executed, e->presents,
                 ev_queued_total((Emu *)e), e->last_api);
    } else {
        snprintf(line, sizeof(line), "%s: %.90s", st,
                 e->state == EMU_CRASHED ? e->crash_msg : "F10=REBOOT");
    }
    line[sizeof(line) - 1] = 0;

    char disp[132];
    size_t n = strlen(line);
    if (n > 131) n = 131;
    for (size_t i = 0; i < n; i++)
        disp[i] = (char)toupper((unsigned char)line[i]);
    disp[n] = 0;
    uint32_t col = e->state == EMU_RUN ? 0xFF30D030u :
                   e->state == EMU_CRASHED ? 0xFFFF4040u : 0xFFFFFF40u;
    draw_text(argb, 3, 1, disp, col);
}

void overlay_crash_text(const Emu *e, char *out, size_t n) {
    snprintf(out, n, "%s", e->crash_msg);
}

void overlay_help(const Emu *e, uint32_t *argb) {
    (void)e;
    static const char *lines[] = {
        "NEETEMULATOR HELP  (F1 TO CLOSE)",
        "",
        "MOUSE: CLICK/DRAG/WHEEL AS ON A NEET SCREEN.",
        "TYPE NORMALLY. KEY REPEAT IS IGNORED.",
        "",
        "F1  THIS HELP",
        "F9  SAVE SNAPSHOT BMP TO SNAPSHOTS/",
        "F10 REBOOT THE MACHINE",
        "F11 FULLSCREEN ON/OFF",
        "CTRL+1 / CTRL+2  WINDOW SIZE 1X / 2X",
        "CTRL+L OVERLAY STATS ON/OFF",
        "",
        "IF THE APP EXITS OR CRASHES, THE LAST FRAME",
        "STAYS ON SCREEN. PRESS F10 TO REBOOT.",
        "SEE THE CONSOLE LOG FOR DETAILS.",
    };
    for (int i = 0; i < NEET_W * NEET_H; i++) {
        uint32_t p = argb[i];
        unsigned r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        argb[i] = 0xFF000000u | ((r / 3) << 16) | ((g / 3) << 8) | (b / 3);
    }
    int y = 60;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++, y += 12) {
        char disp[64];
        size_t n = strlen(lines[i]);
        if (n > 63) n = 63;
        for (size_t k = 0; k < n; k++)
            disp[k] = (char)toupper((unsigned char)lines[i][k]);
        disp[n] = 0;
        uint32_t col = (i == 0) ? 0xFFFFFF40u : 0xFFE8E8E8u;
        draw_text(argb, 60, y, disp, col);
    }
}
