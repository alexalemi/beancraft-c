#define _GNU_SOURCE
#include "beancraft/devices.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
    DEV_INC_LAST = DEV_RAND_SETSEED,
    // deb-polls (`deb R` lets the device refresh its registers first)
    DEV_CON_READ,
    DEV_POLL_LAST = DEV_CON_READ,
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

const char *const *device_dependencies(const char *name, uint32_t *count) {
    switch (magic_op(name)) {
    case DEV_CON_EMIT: case DEV_CON_ERR: case DEV_CON_READ: *count = 1; return DEP_CON;
    case DEV_TIME_NOW:     *count = 8; return DEP_TIME;
    case DEV_RAND_NEXT:    *count = 1; return DEP_RNEXT;
    case DEV_RAND_SETSEED: *count = 1; return DEP_RSEED;
    case DEV_SYS_EXIT:     *count = 1; return DEP_EXIT;
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

    bool *inc_mask;       // sized n
    bool *deb_mask;       // sized n
    int  *op_of;          // sized n: DevOp per register

    // indices of the data registers we care about (-1 if absent)
    int i_sys_code;
    int i_con_byte;
    int i_time_year, i_time_month, i_time_day, i_time_hour, i_time_min, i_time_sec, i_time_dow, i_time_yday;
    int i_rand_byte, i_rand_seed;

    uint64_t rng;         // xorshift64* state (never 0)
} D = {
    .i_sys_code = -1, .i_con_byte = -1,
    .i_time_year = -1, .i_time_month = -1, .i_time_day = -1, .i_time_hour = -1,
    .i_time_min = -1, .i_time_sec = -1, .i_time_dow = -1, .i_time_yday = -1,
    .i_rand_byte = -1, .i_rand_seed = -1,
};

// Set reg[idx] := value (freeing any heap value first).  No-op if idx < 0.
static void set_reg(int idx, uint64_t value) {
    if (idx < 0) return;
    bignum_set_zero(&D.regs[idx]);
    D.regs[idx] = bignum_from_u64(value);
}

// reg[idx] mod 256, then reg[idx] := 0.  Returns 0 if idx < 0.
static uint8_t take_byte(int idx) {
    if (idx < 0) return 0;
    uint8_t b = (uint8_t)bignum_divmod_small(&D.regs[idx], 256);
    bignum_set_zero(&D.regs[idx]);
    return b;
}

// reg[idx] as a u64 (0 if it's bigger than 64 bits, or idx < 0).
static uint64_t reg_u64(int idx) {
    uint64_t v = 0;
    if (idx >= 0) bignum_to_u64(D.regs[idx], &v);
    return v;
}

static uint64_t rng_next(void) {
    uint64_t x = D.rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    D.rng = x;
    return x;
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

    for (uint32_t i = 0; i < reg_count; i++) {
        int op = magic_op(reg_names[i]);
        D.op_of[i] = op;
        if (op_is_inc_trigger(op)) D.inc_mask[i] = true;
        if (op_is_deb_poll(op))    D.deb_mask[i] = true;

        const char *nm = reg_names[i];
        if      (!strcmp(nm, "sys/code"))   D.i_sys_code = (int)i;
        else if (!strcmp(nm, "con/byte"))   D.i_con_byte = (int)i;
        else if (!strcmp(nm, "time/year"))  D.i_time_year = (int)i;
        else if (!strcmp(nm, "time/month")) D.i_time_month = (int)i;
        else if (!strcmp(nm, "time/day"))   D.i_time_day = (int)i;
        else if (!strcmp(nm, "time/hour"))  D.i_time_hour = (int)i;
        else if (!strcmp(nm, "time/min"))   D.i_time_min = (int)i;
        else if (!strcmp(nm, "time/sec"))   D.i_time_sec = (int)i;
        else if (!strcmp(nm, "time/dow"))   D.i_time_dow = (int)i;
        else if (!strcmp(nm, "time/yday"))  D.i_time_yday = (int)i;
        else if (!strcmp(nm, "rand/byte"))  D.i_rand_byte = (int)i;
        else if (!strcmp(nm, "rand/seed"))  D.i_rand_seed = (int)i;
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    D.rng = 0x9e3779b97f4a7c15ULL ^ ((uint64_t)ts.tv_sec << 20) ^ (uint64_t)ts.tv_nsec
          ^ ((uint64_t)getpid() << 40);
    if (D.rng == 0) D.rng = 1;
    return true;
}

void device_shutdown(void) {
    if (!D.active) return;
    fflush(stdout);
    D.active = false;
}

const bool *device_inc_mask(void) { return D.active ? D.inc_mask : NULL; }
const bool *device_deb_mask(void) { return D.active ? D.deb_mask : NULL; }

// ---------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------

void device_on_inc(uint32_t i) {
    switch (D.op_of[i]) {
    case DEV_SYS_HALT:
        device_shutdown();
        exit(0);

    case DEV_SYS_EXIT:
        device_shutdown();
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

    case DEV_CON_EMIT:
        putchar(take_byte(D.i_con_byte));
        break;

    case DEV_CON_ERR:
        fputc(take_byte(D.i_con_byte), stderr);
        break;

    case DEV_TIME_NOW: {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        set_reg(D.i_time_year,  (uint64_t)tm.tm_year + 1900);
        set_reg(D.i_time_month, (uint64_t)tm.tm_mon + 1);
        set_reg(D.i_time_day,   (uint64_t)tm.tm_mday);
        set_reg(D.i_time_hour,  (uint64_t)tm.tm_hour);
        set_reg(D.i_time_min,   (uint64_t)tm.tm_min);
        set_reg(D.i_time_sec,   (uint64_t)tm.tm_sec);
        set_reg(D.i_time_dow,   (uint64_t)tm.tm_wday);
        set_reg(D.i_time_yday,  (uint64_t)tm.tm_yday + 1);
        break;
    }

    case DEV_RAND_NEXT:
        set_reg(D.i_rand_byte, rng_next() & 0xff);
        break;

    case DEV_RAND_SETSEED: {
        uint64_t s = reg_u64(D.i_rand_seed);
        D.rng = s ? s : 1;
        break;
    }
    }
}

void device_on_deb(uint32_t i) {
    switch (D.op_of[i]) {
    case DEV_CON_READ: {
        int c = getchar();
        if (c == EOF) {
            set_reg((int)i, 0);          // con/read = 0  -> the deb branches to "eof"
        } else {
            set_reg(D.i_con_byte, (uint64_t)c);
            set_reg((int)i, 1);          // con/read = 1  -> the deb consumes it, falls through
        }
        break;
    }
    }
}
