#define _GNU_SOURCE
#include "beancraft/devices.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>

// ---------------------------------------------------------------------------
// Magic-register catalogue
// ---------------------------------------------------------------------------

// What a register's name means to the runtime.
enum DevOp {
    DEV_NONE = 0,        // not a magic register
    DEV_DATA,            // a data register the runtime reads/writes (normal inc/deb)
    // inc-triggers (`inc R` fires the side effect; the register isn't actually incremented)
    DEV_SYS_HALT, DEV_SYS_EXIT, DEV_SYS_DEBUG,
    DEV_CON_EMIT, DEV_CON_ERR,
    DEV_TIME_NOW,
    DEV_RAND_NEXT, DEV_RAND_SETSEED,
    DEV_PAL_SET,
    DEV_SCR_PLOT, DEV_SCR_CLEAR, DEV_SCR_FLUSH,
    DEV_INC_LAST = DEV_SCR_FLUSH,
    // deb-polls (`deb R` lets the device refresh its registers first)
    DEV_CON_READ, DEV_KBD_EVENT,
    DEV_POLL_LAST = DEV_KBD_EVENT,
};

static bool op_is_inc_trigger(int op) { return op >= DEV_SYS_HALT && op <= DEV_INC_LAST; }
static bool op_is_deb_poll(int op)    { return op >  DEV_INC_LAST && op <= DEV_POLL_LAST; }

static const struct { const char *name; int op; } MAGIC[] = {
    { "sys/halt",     DEV_SYS_HALT     },
    { "sys/code",     DEV_DATA         },
    { "sys/exit",     DEV_SYS_EXIT     },
    { "sys/debug",    DEV_SYS_DEBUG    },
    { "con/byte",     DEV_DATA         },
    { "con/emit",     DEV_CON_EMIT     },
    { "con/err",      DEV_CON_ERR      },
    { "con/read",     DEV_CON_READ     },
    { "time/now",     DEV_TIME_NOW     },
    { "time/year",    DEV_DATA }, { "time/month", DEV_DATA }, { "time/day",  DEV_DATA },
    { "time/hour",    DEV_DATA }, { "time/min",   DEV_DATA }, { "time/sec",  DEV_DATA },
    { "time/dow",     DEV_DATA }, { "time/yday",  DEV_DATA },
    { "rand/next",    DEV_RAND_NEXT    },
    { "rand/byte",    DEV_DATA         },
    { "rand/seed",    DEV_DATA         },
    { "rand/setseed", DEV_RAND_SETSEED },
    { "palette/index", DEV_DATA }, { "palette/r", DEV_DATA }, { "palette/g", DEV_DATA }, { "palette/b", DEV_DATA },
    { "palette/set",  DEV_PAL_SET      },
    { "screen/x",     DEV_DATA }, { "screen/y", DEV_DATA }, { "screen/color", DEV_DATA },
    { "screen/width", DEV_DATA }, { "screen/height", DEV_DATA },
    { "screen/plot",  DEV_SCR_PLOT     },
    { "screen/clear", DEV_SCR_CLEAR    },
    { "screen/flush", DEV_SCR_FLUSH    },
    { "kbd/event",    DEV_KBD_EVENT    },
    { "kbd/char",     DEV_DATA         },
};
#define N_MAGIC (sizeof(MAGIC) / sizeof(MAGIC[0]))

static int magic_op(const char *name) {
    for (size_t i = 0; i < N_MAGIC; i++) {
        if (strcmp(MAGIC[i].name, name) == 0) return MAGIC[i].op;
    }
    return DEV_NONE;
}

bool device_name_is_inc_trigger(const char *name) { return op_is_inc_trigger(magic_op(name)); }
bool device_name_is_deb_poll(const char *name)    { return op_is_deb_poll(magic_op(name)); }
bool device_name_is_known(const char *name)       { return magic_op(name) != DEV_NONE; }

static const char *const DEP_CON[]  = { "con/byte" };
static const char *const DEP_TIME[] = { "time/year", "time/month", "time/day", "time/hour",
                                        "time/min", "time/sec", "time/dow", "time/yday" };
static const char *const DEP_RNEXT[] = { "rand/byte" };
static const char *const DEP_RSEED[] = { "rand/seed" };
static const char *const DEP_EXIT[]  = { "sys/code" };
static const char *const DEP_PALSET[] = { "palette/index", "palette/r", "palette/g", "palette/b" };
static const char *const DEP_PLOT[]  = { "screen/x", "screen/y", "screen/color" };
static const char *const DEP_KBD[]   = { "kbd/char" };
static const char *const DEP_SCRSZ[] = { "screen/width", "screen/height" };

const char *const *device_dependencies(const char *name, uint32_t *count) {
    switch (magic_op(name)) {
    case DEV_CON_EMIT: case DEV_CON_ERR: case DEV_CON_READ: *count = 1; return DEP_CON;
    case DEV_TIME_NOW:     *count = 8; return DEP_TIME;
    case DEV_RAND_NEXT:    *count = 1; return DEP_RNEXT;
    case DEV_RAND_SETSEED: *count = 1; return DEP_RSEED;
    case DEV_SYS_EXIT:     *count = 1; return DEP_EXIT;
    case DEV_PAL_SET:      *count = 4; return DEP_PALSET;
    case DEV_SCR_PLOT:     *count = 3; return DEP_PLOT;
    case DEV_KBD_EVENT:    *count = 1; return DEP_KBD;
    // Any screen op also wants the size registers so the program can read them.
    case DEV_SCR_CLEAR: case DEV_SCR_FLUSH: *count = 2; return DEP_SCRSZ;
    default:               *count = 0; return NULL;
    }
}

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------

static struct {
    bool active;
    const char **names;
    Bignum *regs;
    uint32_t n;

    bool *inc_mask, *deb_mask;
    int  *op_of;          // DevOp per register

    // data-register indices (-1 if absent)
    int i_sys_code, i_con_byte;
    int i_time[8];        // year, month, day, hour, min, sec, dow, yday
    int i_rand_byte, i_rand_seed;
    int i_pal_index, i_pal_r, i_pal_g, i_pal_b;
    int i_scr_x, i_scr_y, i_scr_color, i_scr_w, i_scr_h;
    int i_kbd_char;

    uint64_t rng;

    // screen
    bool have_screen;
    uint8_t *fb;          // scr_w * scr_h palette indices
    uint32_t scr_w, scr_h;
    uint32_t pal[256];    // 0x00RRGGBB
    struct timespec last_flush;
    bool alt_screen;

    // keyboard / tty
    bool kbd_raw;
    struct termios saved_tio;
    bool tio_saved;
    uint8_t kq[256];      // ring buffer of pending key bytes
    int kq_head, kq_tail;
} D = {
    .i_sys_code = -1, .i_con_byte = -1,
    .i_time = { -1, -1, -1, -1, -1, -1, -1, -1 },
    .i_rand_byte = -1, .i_rand_seed = -1,
    .i_pal_index = -1, .i_pal_r = -1, .i_pal_g = -1, .i_pal_b = -1,
    .i_scr_x = -1, .i_scr_y = -1, .i_scr_color = -1, .i_scr_w = -1, .i_scr_h = -1,
    .i_kbd_char = -1,
};

static void set_reg(int idx, uint64_t value) {
    if (idx < 0) return;
    bignum_set_zero(&D.regs[idx]);
    D.regs[idx] = bignum_from_u64(value);
}
static uint8_t take_byte(int idx) {
    if (idx < 0) return 0;
    uint8_t b = (uint8_t)bignum_divmod_small(&D.regs[idx], 256);
    bignum_set_zero(&D.regs[idx]);
    return b;
}
static uint64_t reg_u64(int idx) {
    uint64_t v = 0;
    if (idx >= 0) bignum_to_u64(D.regs[idx], &v);
    return v;
}
static uint64_t reg_mod(int idx, uint64_t m) {
    if (idx < 0) return 0;
    Bignum tmp = bignum_clone(D.regs[idx]);
    uint64_t r = bignum_divmod_small(&tmp, m);
    bignum_free(&tmp);
    return r;
}
static uint64_t rng_next(void) {
    uint64_t x = D.rng;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    D.rng = x;
    return x;
}

// ---------------------------------------------------------------------------
// Screen / keyboard helpers
// ---------------------------------------------------------------------------

// The standard xterm 256-colour layout: 16 system colours, a 6x6x6 cube, 24 greys.
static void init_palette(void) {
    static const uint32_t sys16[16] = {
        0x000000, 0xaa0000, 0x00aa00, 0xaa5500, 0x0000aa, 0xaa00aa, 0x00aaaa, 0xaaaaaa,
        0x555555, 0xff5555, 0x55ff55, 0xffff55, 0x5555ff, 0xff55ff, 0x55ffff, 0xffffff,
    };
    for (int i = 0; i < 16; i++) D.pal[i] = sys16[i];
    for (int i = 0; i < 216; i++) {
        int r6 = i / 36, g6 = (i / 6) % 6, b6 = i % 6;
        int r = r6 ? 55 + r6 * 40 : 0, g = g6 ? 55 + g6 * 40 : 0, b = b6 ? 55 + b6 * 40 : 0;
        D.pal[16 + i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    for (int i = 0; i < 24; i++) {
        int v = 8 + i * 10;
        D.pal[232 + i] = ((uint32_t)v << 16) | ((uint32_t)v << 8) | (uint32_t)v;
    }
}

static void kq_push(uint8_t b) {
    int next = (D.kq_tail + 1) & 0xff;
    if (next != D.kq_head) { D.kq[D.kq_tail] = b; D.kq_tail = next; }  // drop on overflow
}
static int kq_pop(void) {  // -1 if empty
    if (D.kq_head == D.kq_tail) return -1;
    uint8_t b = D.kq[D.kq_head];
    D.kq_head = (D.kq_head + 1) & 0xff;
    return b;
}
static void drain_keys(void) {
    if (!D.kbd_raw) return;
    uint8_t buf[128];
    ssize_t n;
    while ((n = read(STDIN_FILENO, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; i++) kq_push(buf[i]);
    }
}

// Render the framebuffer to the terminal: two stacked pixels per cell, drawn
// with U+2580 "upper half block" (foreground = top pixel, background = bottom).
static void screen_render(void) {
    static char *out = NULL;
    static size_t cap = 0;
    size_t need = (size_t)D.scr_w * D.scr_h * 40 + 64;
    if (need > cap) { out = realloc(out, need); cap = need; }
    char *p = out;
    p += sprintf(p, "\x1b[H");                           // cursor home (no clear -> no flicker)
    int last_fg = -1, last_bg = -1;
    for (uint32_t cy = 0; cy * 2 < D.scr_h; cy++) {
        if (cy) { p += sprintf(p, "\x1b[0m\r\n"); last_fg = last_bg = -1; }
        uint32_t y0 = cy * 2, y1 = y0 + 1;
        for (uint32_t x = 0; x < D.scr_w; x++) {
            uint32_t fg = D.pal[D.fb[y0 * D.scr_w + x]];
            uint32_t bg = (y1 < D.scr_h) ? D.pal[D.fb[y1 * D.scr_w + x]] : 0;
            if ((int)fg != last_fg) { p += sprintf(p, "\x1b[38;2;%u;%u;%um", fg >> 16 & 0xff, fg >> 8 & 0xff, fg & 0xff); last_fg = (int)fg; }
            if ((int)bg != last_bg) { p += sprintf(p, "\x1b[48;2;%u;%u;%um", bg >> 16 & 0xff, bg >> 8 & 0xff, bg & 0xff); last_bg = (int)bg; }
            *p++ = '\xe2'; *p++ = '\x96'; *p++ = '\x80';  // UTF-8 for U+2580
        }
    }
    p += sprintf(p, "\x1b[0m");
    fwrite(out, 1, (size_t)(p - out), stdout);
    fflush(stdout);
}

static void screen_vsync(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long target = (long long)D.last_flush.tv_sec * 1000000000LL + D.last_flush.tv_nsec + 16666667LL;
    long long cur    = (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
    if (cur < target) {
        struct timespec d = { (target - cur) / 1000000000LL, (target - cur) % 1000000000LL };
        nanosleep(&d, NULL);
        clock_gettime(CLOCK_MONOTONIC, &D.last_flush);
    } else {
        D.last_flush = now;   // running behind: don't try to catch up
    }
}

// ---------------------------------------------------------------------------
// Setup / teardown
// ---------------------------------------------------------------------------

bool device_init(const char **reg_names, uint32_t reg_count, Bignum *regs) {
    bool any = false;
    for (uint32_t i = 0; i < reg_count && !any; i++) any = device_name_is_known(reg_names[i]);
    if (!any) return false;

    D.active = true;
    D.names = reg_names;
    D.regs = regs;
    D.n = reg_count;
    D.inc_mask = calloc(reg_count, sizeof(bool));
    D.deb_mask = calloc(reg_count, sizeof(bool));
    D.op_of = malloc(reg_count * sizeof(int));

    bool wants_screen = false, wants_kbd = false;
    for (uint32_t i = 0; i < reg_count; i++) {
        int op = magic_op(reg_names[i]);
        D.op_of[i] = op;
        if (op_is_inc_trigger(op)) D.inc_mask[i] = true;
        if (op_is_deb_poll(op))    D.deb_mask[i] = true;
        if (op != DEV_NONE && strncmp(reg_names[i], "screen/", 7) == 0) wants_screen = true;
        if (op != DEV_NONE && strncmp(reg_names[i], "palette/", 8) == 0) wants_screen = true;
        if (op == DEV_KBD_EVENT || (op == DEV_DATA && strncmp(reg_names[i], "kbd/", 4) == 0)) wants_kbd = true;

        const char *nm = reg_names[i];
        if      (!strcmp(nm, "sys/code"))      D.i_sys_code = (int)i;
        else if (!strcmp(nm, "con/byte"))      D.i_con_byte = (int)i;
        else if (!strcmp(nm, "time/year"))     D.i_time[0] = (int)i;
        else if (!strcmp(nm, "time/month"))    D.i_time[1] = (int)i;
        else if (!strcmp(nm, "time/day"))      D.i_time[2] = (int)i;
        else if (!strcmp(nm, "time/hour"))     D.i_time[3] = (int)i;
        else if (!strcmp(nm, "time/min"))      D.i_time[4] = (int)i;
        else if (!strcmp(nm, "time/sec"))      D.i_time[5] = (int)i;
        else if (!strcmp(nm, "time/dow"))      D.i_time[6] = (int)i;
        else if (!strcmp(nm, "time/yday"))     D.i_time[7] = (int)i;
        else if (!strcmp(nm, "rand/byte"))     D.i_rand_byte = (int)i;
        else if (!strcmp(nm, "rand/seed"))     D.i_rand_seed = (int)i;
        else if (!strcmp(nm, "palette/index")) D.i_pal_index = (int)i;
        else if (!strcmp(nm, "palette/r"))     D.i_pal_r = (int)i;
        else if (!strcmp(nm, "palette/g"))     D.i_pal_g = (int)i;
        else if (!strcmp(nm, "palette/b"))     D.i_pal_b = (int)i;
        else if (!strcmp(nm, "screen/x"))      D.i_scr_x = (int)i;
        else if (!strcmp(nm, "screen/y"))      D.i_scr_y = (int)i;
        else if (!strcmp(nm, "screen/color"))  D.i_scr_color = (int)i;
        else if (!strcmp(nm, "screen/width"))  D.i_scr_w = (int)i;
        else if (!strcmp(nm, "screen/height")) D.i_scr_h = (int)i;
        else if (!strcmp(nm, "kbd/char"))      D.i_kbd_char = (int)i;
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    D.rng = 0x9e3779b97f4a7c15ULL ^ ((uint64_t)ts.tv_sec << 20) ^ (uint64_t)ts.tv_nsec ^ ((uint64_t)getpid() << 40);
    if (D.rng == 0) D.rng = 1;

    if (wants_kbd) {
        // On a real terminal, switch to raw, no-echo input; from a pipe/file
        // there's no termios to set, but we still want non-blocking reads.
        if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &D.saved_tio) == 0) {
            D.tio_saved = true;
            struct termios raw = D.saved_tio;
            raw.c_lflag &= ~(unsigned)(ICANON | ECHO);   // keep ISIG so Ctrl-C still quits
            raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
        int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (fl != -1) fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
        D.kbd_raw = true;
    }

    if (wants_screen) {
        D.have_screen = true;
        init_palette();
        uint32_t cols = 64, rows = 24;
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 1) {
            cols = ws.ws_col; rows = ws.ws_row;
        }
        D.scr_w = cols;
        D.scr_h = rows * 2;
        D.fb = calloc((size_t)D.scr_w * D.scr_h, 1);
        set_reg(D.i_scr_w, D.scr_w);
        set_reg(D.i_scr_h, D.scr_h);
        clock_gettime(CLOCK_MONOTONIC, &D.last_flush);
        if (isatty(STDOUT_FILENO)) {
            fputs("\x1b[?1049h\x1b[?25l\x1b[2J", stdout);  // alt screen, hide cursor, clear
            fflush(stdout);
            D.alt_screen = true;
        }
    }

    atexit(device_shutdown);
    return true;
}

void device_shutdown(void) {
    if (!D.active) return;
    D.active = false;   // make idempotent before doing anything that might re-enter
    if (D.alt_screen) { fputs("\x1b[0m\x1b[?25h\x1b[?1049l", stdout); fflush(stdout); }
    if (D.kbd_raw) {
        int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (fl != -1) fcntl(STDIN_FILENO, F_SETFL, fl & ~O_NONBLOCK);
        if (D.tio_saved) tcsetattr(STDIN_FILENO, TCSANOW, &D.saved_tio);
    }
    fflush(stdout);
}

const bool *device_inc_mask(void) { return D.active ? D.inc_mask : NULL; }
const bool *device_deb_mask(void) { return D.active ? D.deb_mask : NULL; }

// ---------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------

void device_on_inc(uint32_t i) {
    switch (D.op_of[i]) {
    case DEV_SYS_HALT:
        exit(0);

    case DEV_SYS_EXIT:
        exit((int)(reg_u64(D.i_sys_code) & 0x7f));

    case DEV_SYS_DEBUG:
        fprintf(stderr, "--- registers ---\n");
        for (uint32_t j = 0; j < D.n; j++) {
            if (D.names[j][0] == ':') continue;
            char *s = bignum_to_string(D.regs[j]);
            fprintf(stderr, "  %s = %s\n", D.names[j], s);
            free(s);
        }
        break;

    case DEV_CON_EMIT: putchar(take_byte(D.i_con_byte)); break;
    case DEV_CON_ERR:  fputc(take_byte(D.i_con_byte), stderr); break;

    case DEV_TIME_NOW: {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        set_reg(D.i_time[0], (uint64_t)tm.tm_year + 1900);
        set_reg(D.i_time[1], (uint64_t)tm.tm_mon + 1);
        set_reg(D.i_time[2], (uint64_t)tm.tm_mday);
        set_reg(D.i_time[3], (uint64_t)tm.tm_hour);
        set_reg(D.i_time[4], (uint64_t)tm.tm_min);
        set_reg(D.i_time[5], (uint64_t)tm.tm_sec);
        set_reg(D.i_time[6], (uint64_t)tm.tm_wday);
        set_reg(D.i_time[7], (uint64_t)tm.tm_yday + 1);
        break;
    }

    case DEV_RAND_NEXT: set_reg(D.i_rand_byte, rng_next() & 0xff); break;
    case DEV_RAND_SETSEED: { uint64_t s = reg_u64(D.i_rand_seed); D.rng = s ? s : 1; break; }

    case DEV_PAL_SET: {
        uint64_t idx = reg_mod(D.i_pal_index, 256);
        uint64_t r = reg_mod(D.i_pal_r, 256), g = reg_mod(D.i_pal_g, 256), b = reg_mod(D.i_pal_b, 256);
        D.pal[idx] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        break;
    }

    case DEV_SCR_PLOT:
        if (D.fb) {
            uint64_t x = reg_mod(D.i_scr_x, D.scr_w);
            uint64_t y = reg_mod(D.i_scr_y, D.scr_h);
            D.fb[y * D.scr_w + x] = (uint8_t)reg_mod(D.i_scr_color, 256);
        }
        break;

    case DEV_SCR_CLEAR:
        if (D.fb) memset(D.fb, 0, (size_t)D.scr_w * D.scr_h);
        break;

    case DEV_SCR_FLUSH:
        drain_keys();
        if (D.have_screen) {
            if (D.alt_screen) screen_render();
            screen_vsync();
        }
        break;
    }
}

void device_on_deb(uint32_t i) {
    switch (D.op_of[i]) {
    case DEV_CON_READ: {
        int c = getchar();
        if (c == EOF) {
            set_reg((int)i, 0);                     // con/read = 0 -> deb -> "eof"
        } else {
            set_reg(D.i_con_byte, (uint64_t)c);
            set_reg((int)i, 1);                     // con/read = 1 -> deb consumes, "got"
        }
        break;
    }
    case DEV_KBD_EVENT: {
        drain_keys();
        int c = kq_pop();
        if (c < 0) {
            set_reg((int)i, 0);                     // no key -> deb -> "none"
        } else {
            set_reg(D.i_kbd_char, (uint64_t)c);
            set_reg((int)i, 1);                     // a key -> deb consumes, "got"
        }
        break;
    }
    }
}
