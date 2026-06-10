#ifndef BC_DEVICES_H
#define BC_DEVICES_H

// "Devices" expose host I/O (console, clock, randomness, and later screen,
// keyboard, mouse) as magic registers whose names contain '/' -- e.g. con/byte,
// con/emit, time/now, rand/byte. The language stays inc/deb/end: the only
// special behaviour is that, for a *trigger* register, `inc R` fires a side
// effect (and doesn't actually increment), and for a *poll* register, `deb R`
// first lets the device load fresh data into its registers. Plain data
// registers (con/byte, time/year, ...) are ordinary registers that the runtime
// happens to read or write at those moments.

#include "bignum.h"
#include <stdint.h>
#include <stdbool.h>

// Name classification (used by the QBE backend and the optimizer at compile
// time; the interpreter uses the masks from device_init instead).
bool device_name_is_inc_trigger(const char *name);  // `inc R` -> side effect
bool device_name_is_deb_poll(const char *name);     // `deb R` -> poll first
bool device_name_is_known(const char *name);        // any magic register

// The data registers a magic register implicitly needs (e.g. con/emit needs
// con/byte; time/now needs time/year..time/yday). IR lowering adds these so the
// runtime has somewhere to put the data even if the program never names them.
// Returns an array of *count names (count may be 0).
const char *const *device_dependencies(const char *name, uint32_t *count);

// Wire up the device subsystem for a program whose `reg_count` registers have
// the given names and live in `regs`. Initialises only the devices whose
// registers appear. Returns true iff the program references any magic register.
bool device_init(const char **reg_names, uint32_t reg_count, Bignum *regs);
void device_shutdown(void);

// Per-register masks for the interpreter's hot loop (sized reg_count, or NULL if
// no devices). inc_mask[i] => `inc reg[i]` must call device_on_inc(i) instead of
// bignum_inc; deb_mask[i] => `deb reg[i]` must call device_on_deb(i) before the
// usual decrement.
const bool *device_inc_mask(void);
const bool *device_deb_mask(void);

void device_on_inc(uint32_t reg_index);  // run the trigger for reg[reg_index]
void device_on_deb(uint32_t reg_index);  // run the poll for reg[reg_index]

// Read-only view of the screen back buffer (palette indices, row-major) and
// the 256-entry 0xRRGGBB palette. Valid until device_shutdown; fb is NULL if
// no screen device is active. Used by the wasm host to draw the final frame.
const uint8_t *device_screen_fb(uint32_t *w, uint32_t *h);
const uint32_t *device_screen_palette(void);

#endif // BC_DEVICES_H
