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
#ifdef BC_SDL
#include <SDL2/SDL.h>
#endif
#include "font8x8.h"

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
    DEV_SCR_SPRITE, DEV_SCR_RECT, DEV_SCR_TEXT, DEV_SCR_SAMPLE,
    DEV_AUDIO_PLAY,
    DEV_INC_LAST = DEV_AUDIO_PLAY,
    // deb-polls (`deb R` lets the device refresh its registers first)
    DEV_CON_READ, DEV_KBD_EVENT, DEV_MOUSE_EVENT,
    DEV_POLL_LAST = DEV_MOUSE_EVENT,
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
    { "screen/row0",  DEV_DATA }, { "screen/row1", DEV_DATA }, { "screen/row2", DEV_DATA }, { "screen/row3", DEV_DATA },
    { "screen/row4",  DEV_DATA }, { "screen/row5", DEV_DATA }, { "screen/row6", DEV_DATA }, { "screen/row7", DEV_DATA },
    { "screen/sprite", DEV_SCR_SPRITE  },
    { "screen/rectw", DEV_DATA }, { "screen/recth", DEV_DATA },
    { "screen/rect",  DEV_SCR_RECT     },
    { "screen/glyph", DEV_DATA },
    { "screen/text",  DEV_SCR_TEXT     },
    { "screen/pixel", DEV_DATA },
    { "screen/sample", DEV_SCR_SAMPLE  },
    { "kbd/event",    DEV_KBD_EVENT    },
    { "kbd/char",     DEV_DATA }, { "kbd/code", DEV_DATA },
    { "mouse/event",  DEV_MOUSE_EVENT  },
    { "mouse/x",      DEV_DATA }, { "mouse/y", DEV_DATA }, { "mouse/buttons", DEV_DATA },
    { "audio/freq",   DEV_DATA }, { "audio/duration", DEV_DATA },
    { "audio/play",   DEV_AUDIO_PLAY   },
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

static const char *const DEP_CON[]   = { "con/byte" };
static const char *const DEP_TIME[]  = { "time/year", "time/month", "time/day", "time/hour",
                                         "time/min", "time/sec", "time/dow", "time/yday" };
static const char *const DEP_RNEXT[] = { "rand/byte" };
static const char *const DEP_RSEED[] = { "rand/seed" };
static const char *const DEP_EXIT[]  = { "sys/code" };
static const char *const DEP_PALSET[] = { "palette/index", "palette/r", "palette/g", "palette/b" };
static const char *const DEP_PLOT[]  = { "screen/x", "screen/y", "screen/color" };
static const char *const DEP_KBD[]   = { "kbd/char", "kbd/code" };
static const char *const DEP_MOUSE[] = { "mouse/x", "mouse/y", "mouse/buttons" };
static const char *const DEP_SCRSZ[] = { "screen/width", "screen/height" };
static const char *const DEP_SPRITE[] = { "screen/x", "screen/y", "screen/color",
    "screen/row0", "screen/row1", "screen/row2", "screen/row3",
    "screen/row4", "screen/row5", "screen/row6", "screen/row7" };
static const char *const DEP_RECT[]  = { "screen/x", "screen/y", "screen/color", "screen/rectw", "screen/recth" };
static const char *const DEP_TEXT[]  = { "screen/x", "screen/y", "screen/color", "screen/glyph" };
static const char *const DEP_SAMPLE[] = { "screen/x", "screen/y", "screen/pixel" };
static const char *const DEP_AUDIO[]  = { "audio/freq", "audio/duration" };

const char *const *device_dependencies(const char *name, uint32_t *count) {
    switch (magic_op(name)) {
    case DEV_CON_EMIT: case DEV_CON_ERR: case DEV_CON_READ: *count = 1; return DEP_CON;
    case DEV_TIME_NOW:     *count = 8; return DEP_TIME;
    case DEV_RAND_NEXT:    *count = 1; return DEP_RNEXT;
    case DEV_RAND_SETSEED: *count = 1; return DEP_RSEED;
    case DEV_SYS_EXIT:     *count = 1; return DEP_EXIT;
    case DEV_PAL_SET:      *count = 4; return DEP_PALSET;
    case DEV_SCR_PLOT:     *count = 3; return DEP_PLOT;
    case DEV_SCR_SPRITE:   *count = 11; return DEP_SPRITE;
    case DEV_SCR_RECT:     *count = 5; return DEP_RECT;
    case DEV_SCR_TEXT:     *count = 4; return DEP_TEXT;
    case DEV_SCR_SAMPLE:   *count = 3; return DEP_SAMPLE;
    case DEV_AUDIO_PLAY:   *count = 2; return DEP_AUDIO;
    case DEV_KBD_EVENT:    *count = 2; return DEP_KBD;
    case DEV_MOUSE_EVENT:  *count = 3; return DEP_MOUSE;
    case DEV_SCR_CLEAR: case DEV_SCR_FLUSH: *count = 2; return DEP_SCRSZ;
    default:               *count = 0; return NULL;
    }
}

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------

typedef struct {
    bool active;
    const char **names;
    Bignum *regs;
    uint32_t n;

    bool *inc_mask, *deb_mask;
    int  *op_of;

    int i_sys_code, i_con_byte;
    int i_time[8];
    int i_rand_byte, i_rand_seed;
    int i_pal_index, i_pal_r, i_pal_g, i_pal_b;
    int i_scr_x, i_scr_y, i_scr_color, i_scr_w, i_scr_h;
    int i_scr_row[8], i_scr_rectw, i_scr_recth, i_scr_glyph, i_scr_pixel;
    int i_kbd_char, i_kbd_code;
    int i_mouse_x, i_mouse_y, i_mouse_buttons;
    int i_audio_freq, i_audio_dur;

    uint64_t rng;

    // screen (one back buffer of palette indices; only displayed on flush)
    bool have_screen;
    uint8_t *fb;
    uint32_t scr_w, scr_h;
    uint32_t pal[256];
    struct timespec last_flush;
    bool alt_screen;             // terminal renderer active
    bool sdl_active;             // SDL renderer active (only ever true with BC_SDL)

    // keyboard / tty
    bool kbd_raw;                // raw/non-blocking stdin (terminal kbd path)
    struct termios saved_tio;
    bool tio_saved;
    struct { uint8_t ch, code; } kq[256];
    int kq_head, kq_tail;

    // mouse (filled by the SDL event pump; the terminal backend never updates it)
    uint32_t mouse_x, mouse_y, mouse_buttons;
    bool mouse_changed;

#ifdef BC_SDL
    SDL_Window   *sdl_win;
    SDL_Renderer *sdl_ren;
    SDL_Texture  *sdl_tex;
    uint32_t     *sdl_rgb;       // scr_w*scr_h ARGB scratch for the texture
    SDL_AudioDeviceID sdl_audio; // 0 until/unless the audio device opens
#endif
} DevState;

// Shared between the static definition and the device_shutdown reset, so a
// later device_init (e.g. the next run in the wasm playground) starts clean.
#define DEV_STATE_INIT {                                                      \
    .i_sys_code = -1, .i_con_byte = -1,                                       \
    .i_time = { -1, -1, -1, -1, -1, -1, -1, -1 },                             \
    .i_rand_byte = -1, .i_rand_seed = -1,                                     \
    .i_pal_index = -1, .i_pal_r = -1, .i_pal_g = -1, .i_pal_b = -1,           \
    .i_scr_x = -1, .i_scr_y = -1, .i_scr_color = -1, .i_scr_w = -1, .i_scr_h = -1, \
    .i_scr_row = { -1, -1, -1, -1, -1, -1, -1, -1 }, .i_scr_rectw = -1, .i_scr_recth = -1, .i_scr_glyph = -1, .i_scr_pixel = -1, \
    .i_kbd_char = -1, .i_kbd_code = -1,                                       \
    .i_mouse_x = -1, .i_mouse_y = -1, .i_mouse_buttons = -1,                  \
    .i_audio_freq = -1, .i_audio_dur = -1,                                    \
}

static DevState D = DEV_STATE_INIT;

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
// Screen / keyboard helpers (shared)
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

static void kq_push(uint8_t ch, uint8_t code) {
    int next = (D.kq_tail + 1) & 0xff;
    if (next != D.kq_head) { D.kq[D.kq_tail].ch = ch; D.kq[D.kq_tail].code = code; D.kq_tail = next; }
}
static bool kq_pop(uint8_t *ch, uint8_t *code) {
    if (D.kq_head == D.kq_tail) return false;
    *ch = D.kq[D.kq_head].ch; *code = D.kq[D.kq_head].code;
    D.kq_head = (D.kq_head + 1) & 0xff;
    return true;
}
// Non-blocking read of all pending stdin bytes into the key queue (terminal path).
static void drain_keys(void) {
    if (!D.kbd_raw) return;
    uint8_t buf[128];
    ssize_t nr;
    while ((nr = read(STDIN_FILENO, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < nr; i++) kq_push(buf[i], buf[i]);
    }
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
        D.last_flush = now;
    }
}

// Blit an 8x8 1-bit bitmap into the back buffer at top-left (x, y): for each
// set bit, fb := color (bit 0 of each row byte is the leftmost pixel); clear
// bits are transparent. Pixels past the screen edge are dropped (no wrap).
static void blit_bitmap(uint32_t x, uint32_t y, const uint8_t rows[8], uint8_t color) {
    if (!D.fb) return;
    for (uint32_t r = 0; r < 8 && y + r < D.scr_h; r++) {
        uint8_t bits = rows[r];
        for (uint32_t c = 0; c < 8 && x + c < D.scr_w; c++) {
            if (bits & (1u << c)) D.fb[(y + r) * D.scr_w + (x + c)] = color;
        }
    }
}

// Fill a w x h rectangle of the back buffer at top-left (x, y) with `color`,
// clipped to the screen (so huge w/h cost only the visible part).
static void fill_rect(uint32_t x, uint32_t y, uint64_t w, uint64_t h, uint8_t color) {
    if (!D.fb) return;
    for (uint64_t dy = 0; dy < h && y + dy < D.scr_h; dy++) {
        for (uint64_t dx = 0; dx < w && x + dx < D.scr_w; dx++) {
            D.fb[(y + dy) * D.scr_w + (x + dx)] = color;
        }
    }
}

// ---------------------------------------------------------------------------
// Terminal screen renderer (no dependencies)
// ---------------------------------------------------------------------------

// Two stacked pixels per text cell, drawn with U+2580 "upper half block"
// (foreground = top pixel, background = bottom), SGR codes coalesced into runs.
static void term_render(void) {
    static char *out = NULL;
    static size_t cap = 0;
    size_t need = (size_t)D.scr_w * D.scr_h * 40 + 64;
    if (need > cap) { out = realloc(out, need); cap = need; }
    char *p = out;
    p += sprintf(p, "\x1b[H");
    int last_fg = -1, last_bg = -1;
    for (uint32_t cy = 0; cy * 2 < D.scr_h; cy++) {
        if (cy) { p += sprintf(p, "\x1b[0m\r\n"); last_fg = last_bg = -1; }
        uint32_t y0 = cy * 2, y1 = y0 + 1;
        for (uint32_t x = 0; x < D.scr_w; x++) {
            uint32_t fg = D.pal[D.fb[y0 * D.scr_w + x]];
            uint32_t bg = (y1 < D.scr_h) ? D.pal[D.fb[y1 * D.scr_w + x]] : 0;
            if ((int)fg != last_fg) { p += sprintf(p, "\x1b[38;2;%u;%u;%um", fg >> 16 & 0xff, fg >> 8 & 0xff, fg & 0xff); last_fg = (int)fg; }
            if ((int)bg != last_bg) { p += sprintf(p, "\x1b[48;2;%u;%u;%um", bg >> 16 & 0xff, bg >> 8 & 0xff, bg & 0xff); last_bg = (int)bg; }
            *p++ = '\xe2'; *p++ = '\x96'; *p++ = '\x80';   // UTF-8 for U+2580
        }
    }
    p += sprintf(p, "\x1b[0m");
    fwrite(out, 1, (size_t)(p - out), stdout);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// SDL screen renderer (opt-in: build with -DBC_SDL)
// ---------------------------------------------------------------------------
#ifdef BC_SDL
#define SDL_LOGICAL_W 256
#define SDL_LOGICAL_H 192

static bool sdl_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");   // nearest-neighbour scaling
    D.sdl_win = SDL_CreateWindow("beancraft", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 SDL_LOGICAL_W * 3, SDL_LOGICAL_H * 3, SDL_WINDOW_RESIZABLE);
    if (!D.sdl_win) { SDL_Quit(); return false; }
    D.sdl_ren = SDL_CreateRenderer(D.sdl_win, -1, 0);
    if (!D.sdl_ren) { SDL_DestroyWindow(D.sdl_win); SDL_Quit(); return false; }
    SDL_RenderSetLogicalSize(D.sdl_ren, SDL_LOGICAL_W, SDL_LOGICAL_H);
    D.sdl_tex = SDL_CreateTexture(D.sdl_ren, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, SDL_LOGICAL_W, SDL_LOGICAL_H);
    if (!D.sdl_tex) { SDL_DestroyRenderer(D.sdl_ren); SDL_DestroyWindow(D.sdl_win); SDL_Quit(); return false; }
    D.scr_w = SDL_LOGICAL_W;
    D.scr_h = SDL_LOGICAL_H;
    D.fb = calloc((size_t)D.scr_w * D.scr_h, 1);
    D.sdl_rgb = malloc((size_t)D.scr_w * D.scr_h * 4);
    init_palette();
    clock_gettime(CLOCK_MONOTONIC, &D.last_flush);
    SDL_StartTextInput();
    return true;
}

static void sdl_pump_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            exit(0);
        case SDL_TEXTINPUT:
            for (const char *c = e.text.text; *c; c++) kq_push((uint8_t)*c, (uint8_t)*c);
            break;
        case SDL_KEYDOWN: {
            SDL_Keycode k = e.key.keysym.sym;
            if (k == SDLK_RETURN || k == SDLK_ESCAPE || k == SDLK_BACKSPACE || k == SDLK_TAB || k == SDLK_DELETE)
                kq_push((uint8_t)k, (uint8_t)k);            // ASCII control keys
            else if (k == SDLK_UP)    kq_push(0, 1);        // arrows -> small codes 1..4
            else if (k == SDLK_DOWN)  kq_push(0, 2);
            else if (k == SDLK_LEFT)  kq_push(0, 3);
            else if (k == SDLK_RIGHT) kq_push(0, 4);
            // printable keys arrive via SDL_TEXTINPUT
            break;
        }
        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            int wx, wy;
            uint32_t b = SDL_GetMouseState(&wx, &wy);
            float lx = 0, ly = 0;
            SDL_RenderWindowToLogical(D.sdl_ren, wx, wy, &lx, &ly);
            int li = (int)lx, lj = (int)ly;
            if (li < 0) li = 0; else if (li >= (int)D.scr_w) li = (int)D.scr_w - 1;
            if (lj < 0) lj = 0; else if (lj >= (int)D.scr_h) lj = (int)D.scr_h - 1;
            D.mouse_x = (uint32_t)li; D.mouse_y = (uint32_t)lj;
            D.mouse_buttons = (uint32_t)(((b & SDL_BUTTON_LMASK) ? 1 : 0)
                                       | ((b & SDL_BUTTON_MMASK) ? 2 : 0)
                                       | ((b & SDL_BUTTON_RMASK) ? 4 : 0));
            D.mouse_changed = true;
            break;
        }
        default: break;
        }
    }
}

static void sdl_render(void) {
    size_t n = (size_t)D.scr_w * D.scr_h;
    for (size_t i = 0; i < n; i++) D.sdl_rgb[i] = 0xff000000u | D.pal[D.fb[i]];
    SDL_UpdateTexture(D.sdl_tex, NULL, D.sdl_rgb, (int)(D.scr_w * 4));
    SDL_RenderClear(D.sdl_ren);
    SDL_RenderCopy(D.sdl_ren, D.sdl_tex, NULL, NULL);
    SDL_RenderPresent(D.sdl_ren);
}

// Wait (briefly) for any queued audio to finish playing, then close the device.
static void sdl_audio_close(void) {
    if (!D.sdl_audio) return;
    for (int i = 0; i < 500 && SDL_GetQueuedAudioSize(D.sdl_audio) > 0; i++) SDL_Delay(10);
    SDL_CloseAudioDevice(D.sdl_audio);
    D.sdl_audio = 0;
}

static void sdl_shutdown(void) {
    sdl_audio_close();
    if (D.sdl_tex) SDL_DestroyTexture(D.sdl_tex);
    if (D.sdl_ren) SDL_DestroyRenderer(D.sdl_ren);
    if (D.sdl_win) SDL_DestroyWindow(D.sdl_win);
    SDL_Quit();
}

// Open a mono 16-bit 44.1 kHz output device for the square-wave beeper.
static bool sdl_audio_init(void) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 2048;
    D.sdl_audio = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (!D.sdl_audio) { SDL_QuitSubSystem(SDL_INIT_AUDIO); return false; }
    SDL_PauseAudioDevice(D.sdl_audio, 0);   // start playing (the queue is empty)
    return true;
}

// Queue a square-wave tone of `freq` Hz for `dur` ms (clamped, capped backlog).
static void sdl_audio_tone(uint64_t freq, uint64_t dur) {
    if (!D.sdl_audio || dur == 0) return;
    if (freq < 20) freq = 20;
    if (freq > 12000) freq = 12000;
    if (dur > 4000) dur = 4000;
    if (SDL_GetQueuedAudioSize(D.sdl_audio) > 22050u * sizeof(int16_t)) return;  // ~0.25 s backlog cap
    uint32_t total = (uint32_t)(44100u * dur / 1000u);
    uint32_t half  = (uint32_t)(22050u / freq);   // samples per half-period
    if (half == 0) half = 1;
    int16_t *buf = malloc((size_t)total * sizeof(int16_t));
    if (!buf) return;
    for (uint32_t i = 0; i < total; i++) buf[i] = ((i / half) & 1u) ? -6000 : 6000;
    SDL_QueueAudio(D.sdl_audio, buf, total * sizeof(int16_t));
    free(buf);
}
#endif // BC_SDL

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

    bool wants_screen = false, wants_kbd = false, wants_mouse = false, wants_audio = false;
    for (uint32_t i = 0; i < reg_count; i++) {
        int op = magic_op(reg_names[i]);
        D.op_of[i] = op;
        if (op_is_inc_trigger(op)) D.inc_mask[i] = true;
        if (op_is_deb_poll(op))    D.deb_mask[i] = true;
        if (op != DEV_NONE) {
            if (!strncmp(reg_names[i], "screen/", 7) || !strncmp(reg_names[i], "palette/", 8)) wants_screen = true;
            if (!strncmp(reg_names[i], "kbd/", 4))   wants_kbd = true;
            if (!strncmp(reg_names[i], "mouse/", 6)) wants_mouse = true;
            if (!strncmp(reg_names[i], "audio/", 6)) wants_audio = true;
        }

        const char *nm = reg_names[i];
        if      (!strcmp(nm, "sys/code"))       D.i_sys_code = (int)i;
        else if (!strcmp(nm, "con/byte"))       D.i_con_byte = (int)i;
        else if (!strcmp(nm, "time/year"))      D.i_time[0] = (int)i;
        else if (!strcmp(nm, "time/month"))     D.i_time[1] = (int)i;
        else if (!strcmp(nm, "time/day"))       D.i_time[2] = (int)i;
        else if (!strcmp(nm, "time/hour"))      D.i_time[3] = (int)i;
        else if (!strcmp(nm, "time/min"))       D.i_time[4] = (int)i;
        else if (!strcmp(nm, "time/sec"))       D.i_time[5] = (int)i;
        else if (!strcmp(nm, "time/dow"))       D.i_time[6] = (int)i;
        else if (!strcmp(nm, "time/yday"))      D.i_time[7] = (int)i;
        else if (!strcmp(nm, "rand/byte"))      D.i_rand_byte = (int)i;
        else if (!strcmp(nm, "rand/seed"))      D.i_rand_seed = (int)i;
        else if (!strcmp(nm, "palette/index"))  D.i_pal_index = (int)i;
        else if (!strcmp(nm, "palette/r"))      D.i_pal_r = (int)i;
        else if (!strcmp(nm, "palette/g"))      D.i_pal_g = (int)i;
        else if (!strcmp(nm, "palette/b"))      D.i_pal_b = (int)i;
        else if (!strcmp(nm, "screen/x"))       D.i_scr_x = (int)i;
        else if (!strcmp(nm, "screen/y"))       D.i_scr_y = (int)i;
        else if (!strcmp(nm, "screen/color"))   D.i_scr_color = (int)i;
        else if (!strcmp(nm, "screen/width"))   D.i_scr_w = (int)i;
        else if (!strcmp(nm, "screen/height"))  D.i_scr_h = (int)i;
        else if (!strncmp(nm, "screen/row", 10) && nm[10] >= '0' && nm[10] <= '7' && nm[11] == '\0')
                                                D.i_scr_row[nm[10] - '0'] = (int)i;
        else if (!strcmp(nm, "screen/rectw"))   D.i_scr_rectw = (int)i;
        else if (!strcmp(nm, "screen/recth"))   D.i_scr_recth = (int)i;
        else if (!strcmp(nm, "screen/glyph"))   D.i_scr_glyph = (int)i;
        else if (!strcmp(nm, "screen/pixel"))   D.i_scr_pixel = (int)i;
        else if (!strcmp(nm, "kbd/char"))       D.i_kbd_char = (int)i;
        else if (!strcmp(nm, "kbd/code"))       D.i_kbd_code = (int)i;
        else if (!strcmp(nm, "mouse/x"))        D.i_mouse_x = (int)i;
        else if (!strcmp(nm, "mouse/y"))        D.i_mouse_y = (int)i;
        else if (!strcmp(nm, "mouse/buttons"))  D.i_mouse_buttons = (int)i;
        else if (!strcmp(nm, "audio/freq"))     D.i_audio_freq = (int)i;
        else if (!strcmp(nm, "audio/duration")) D.i_audio_dur = (int)i;
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    D.rng = 0x9e3779b97f4a7c15ULL ^ ((uint64_t)ts.tv_sec << 20) ^ (uint64_t)ts.tv_nsec ^ ((uint64_t)getpid() << 40);
    if (D.rng == 0) D.rng = 1;

#ifdef BC_SDL
    if ((wants_screen || wants_mouse) && sdl_init()) {
        D.sdl_active = true;
        D.have_screen = true;
        set_reg(D.i_scr_w, D.scr_w);
        set_reg(D.i_scr_h, D.scr_h);
    } else
#else
    (void)wants_mouse;   // the mouse device only exists in the SDL build
    (void)wants_audio;   // the audio device only makes sound in the SDL build
#endif
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
            fputs("\x1b[?1049h\x1b[?25l\x1b[2J", stdout);
            fflush(stdout);
            D.alt_screen = true;
        }
    }

    // Terminal keyboard path: only when we're not driving an SDL window.
    if (wants_kbd && !D.sdl_active) {
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

#ifdef BC_SDL
    if (wants_audio) sdl_audio_init();   // if it fails, `inc audio/play` is just a no-op
#endif

    static bool atexit_registered = false;
    if (!atexit_registered) {
        atexit(device_shutdown);
        atexit_registered = true;
    }
    return true;
}

void device_shutdown(void) {
    if (!D.active) return;
    D.active = false;
#ifdef BC_SDL
    if (!D.sdl_active) sdl_audio_close();   // audio-only program: drain & close here
    if (D.sdl_active) {
        sdl_shutdown();
    } else
#endif
    {
        if (D.alt_screen) { fputs("\x1b[0m\x1b[?25h\x1b[?1049l", stdout); fflush(stdout); }
        if (D.kbd_raw) {
            int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
            if (fl != -1) fcntl(STDIN_FILENO, F_SETFL, fl & ~O_NONBLOCK);
            if (D.tio_saved) tcsetattr(STDIN_FILENO, TCSANOW, &D.saved_tio);
        }
        fflush(stdout);
    }
    // Release per-run allocations and reset all state (stale register indices,
    // queued keys, screen flags) so the next device_init starts from scratch.
    free(D.inc_mask);
    free(D.deb_mask);
    free(D.op_of);
    free(D.fb);
    D = (DevState)DEV_STATE_INIT;
}

const bool *device_inc_mask(void) { return D.active ? D.inc_mask : NULL; }
const bool *device_deb_mask(void) { return D.active ? D.deb_mask : NULL; }

// ---------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------

static void pump_input(void) {
#ifdef BC_SDL
    if (D.sdl_active) { sdl_pump_events(); return; }
#endif
    drain_keys();
}

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

    case DEV_SCR_SPRITE: {
        uint8_t rows[8];
        for (int k = 0; k < 8; k++) rows[k] = (uint8_t)reg_mod(D.i_scr_row[k], 256);
        blit_bitmap((uint32_t)reg_mod(D.i_scr_x, D.scr_w ? D.scr_w : 1),
                    (uint32_t)reg_mod(D.i_scr_y, D.scr_h ? D.scr_h : 1),
                    rows, (uint8_t)reg_mod(D.i_scr_color, 256));
        break;
    }

    case DEV_SCR_RECT:
        fill_rect((uint32_t)reg_mod(D.i_scr_x, D.scr_w ? D.scr_w : 1),
                  (uint32_t)reg_mod(D.i_scr_y, D.scr_h ? D.scr_h : 1),
                  reg_u64(D.i_scr_rectw), reg_u64(D.i_scr_recth),
                  (uint8_t)reg_mod(D.i_scr_color, 256));
        break;

    case DEV_SCR_TEXT: {
        static const uint8_t blank8[8] = { 0 };
        uint64_t g = reg_mod(D.i_scr_glyph, 256);
        const uint8_t *rows = (g >= BC_FONT_FIRST && g <= BC_FONT_LAST) ? bc_font8x8[g - BC_FONT_FIRST] : blank8;
        blit_bitmap((uint32_t)reg_mod(D.i_scr_x, D.scr_w ? D.scr_w : 1),
                    (uint32_t)reg_mod(D.i_scr_y, D.scr_h ? D.scr_h : 1),
                    rows, (uint8_t)reg_mod(D.i_scr_color, 256));
        if (D.i_scr_x >= 0) bignum_add_into(&D.regs[D.i_scr_x], bignum_from_u64(8));  // advance for the next glyph
        if (D.i_scr_glyph >= 0) bignum_set_zero(&D.regs[D.i_scr_glyph]);              // consume the glyph
        break;
    }

    case DEV_SCR_SAMPLE: {
        // read back the colour of the pixel at (screen/x, screen/y) into screen/pixel
        uint64_t v = 0;
        if (D.fb) {
            uint64_t x = reg_mod(D.i_scr_x, D.scr_w ? D.scr_w : 1);
            uint64_t y = reg_mod(D.i_scr_y, D.scr_h ? D.scr_h : 1);
            v = D.fb[y * D.scr_w + x];
        }
        set_reg(D.i_scr_pixel, v);
        break;
    }

    case DEV_AUDIO_PLAY:
#ifdef BC_SDL
        sdl_audio_tone(reg_u64(D.i_audio_freq), reg_u64(D.i_audio_dur));
#endif
        break;   // no-op without the SDL build

    case DEV_SCR_FLUSH:
#ifdef BC_SDL
        if (D.sdl_active) { sdl_pump_events(); sdl_render(); screen_vsync(); break; }
#endif
        drain_keys();
        if (D.have_screen) { if (D.alt_screen) term_render(); screen_vsync(); }
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
        pump_input();
        uint8_t ch, code;
        if (kq_pop(&ch, &code)) {
            set_reg(D.i_kbd_char, (uint64_t)ch);
            set_reg(D.i_kbd_code, (uint64_t)code);
            set_reg((int)i, 1);                     // a key -> deb consumes, "got"
        } else {
            set_reg((int)i, 0);                     // no key -> deb -> "none"
        }
        break;
    }
    case DEV_MOUSE_EVENT: {
        pump_input();
        if (D.mouse_changed) {
            D.mouse_changed = false;
            set_reg(D.i_mouse_x, D.mouse_x);
            set_reg(D.i_mouse_y, D.mouse_y);
            set_reg(D.i_mouse_buttons, D.mouse_buttons);
            set_reg((int)i, 1);
        } else {
            set_reg((int)i, 0);
        }
        break;
    }
    }
}
