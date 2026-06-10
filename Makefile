CC = cc
CFLAGS = -O2 -Wall -Wextra -Wpedantic -std=c11 -I include
LDFLAGS =
LDLIBS =

# Debug build
DEBUG_CFLAGS = -g -O0 -DDEBUG -fsanitize=address,undefined -I include
DEBUG_LDFLAGS = -fsanitize=address,undefined

# Opt-in SDL backend (window + mouse + key events) for the Screen device.
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null || echo -lSDL2)

SRCDIR = src
INCDIR = include/beancraft
TESTDIR = test
BUILDDIR = build

SRCS = $(wildcard $(SRCDIR)/*.c)
# Exclude QBE runtime files - compiled separately for QBE-generated executables
SRCS := $(filter-out $(SRCDIR)/qbe_runtime.c $(SRCDIR)/qbe_driver.c,$(SRCS))

# Each configuration compiles into its own object directory so switching
# between `make`, `make debug`/`make test`, and `make sdl` never mixes
# objects built with incompatible flags (ASan, -DBC_SDL, ...).
RELEASE_OBJS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/release/%.o,$(SRCS))
DEBUG_OBJS   = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/debug/%.o,$(SRCS))
SDL_OBJS     = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/sdl/%.o,$(SRCS))
DEPS = $(RELEASE_OBJS:.o=.d) $(DEBUG_OBJS:.o=.d) $(SDL_OBJS:.o=.d)

# Exclude main.o for tests
TEST_LIB_OBJS = $(filter-out $(BUILDDIR)/debug/main.o,$(DEBUG_OBJS))

TARGET = beancraft

.PHONY: all clean debug test sdl wasm

all: $(TARGET)

$(TARGET): $(RELEASE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

debug: $(DEBUG_OBJS)
	$(CC) $(DEBUG_LDFLAGS) -o $(TARGET) $^ $(LDLIBS)

sdl: $(SDL_OBJS)
	$(CC) $(LDFLAGS) -o $(TARGET) $^ $(SDL_LIBS) $(LDLIBS)

$(BUILDDIR)/release/%.o: $(SRCDIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR)/debug/%.o: $(SRCDIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(DEBUG_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR)/sdl/%.o: $(SRCDIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DBC_SDL $(SDL_CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

# Test targets (built with the debug/sanitizer configuration)
TEST_SRCS = $(wildcard $(TESTDIR)/*.c)
TEST_BINS = $(patsubst $(TESTDIR)/%.c,$(BUILDDIR)/debug/%,$(TEST_SRCS))

test: $(TEST_BINS) $(TARGET)
	@for t in $(TEST_BINS); do echo "Running $$t..."; $$t || exit 1; done
	bash $(TESTDIR)/run_examples.sh

$(BUILDDIR)/debug/test_%: $(TESTDIR)/test_%.c $(TEST_LIB_OBJS)
	@mkdir -p $(@D)
	$(CC) $(DEBUG_CFLAGS) $(DEBUG_LDFLAGS) -I include -o $@ $^

clean:
	rm -rf $(BUILDDIR) $(TARGET) libbcruntime.a web/beancraft.mjs web/beancraft.wasm

# WebAssembly build of the interpreter (parser + IR + optimizer + interpreter +
# devices -- no QBE backend, which shells out to qbe/cc). Needs Emscripten on
# PATH (https://emscripten.org/). Produces web/beancraft.mjs + web/beancraft.wasm,
# loaded by web/index.html. Serve the repo over HTTP (e.g. `python3 -m http.server`)
# and open /web/index.html -- WASM won't load over file://.
WASM_LIB = arena str lexer parser ast loader ir opt interp bignum error devices
WASM_SRCS = $(WASM_LIB:%=$(SRCDIR)/%.c) web/wasm_main.c
# ASYNCIFY lets screen/flush call emscripten_sleep, suspending the run so the
# browser can paint the canvas and deliver keyboard events: live animation.
WASM_FLAGS = -O2 -I include \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createBeancraft \
  -sEXIT_RUNTIME=0 -sINVOKE_RUN=0 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web \
  -sASYNCIFY -sASYNCIFY_STACK_SIZE=65536 \
  -sEXPORTED_FUNCTIONS=_bc_run_source,_bc_free,_bc_fb_width,_bc_fb_height,_bc_fb_rgba,_bc_push_key,_bc_request_stop,_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8

wasm:
	@command -v emcc >/dev/null 2>&1 || { \
	  echo "wasm: 'emcc' (Emscripten) not found on PATH -- see https://emscripten.org/docs/getting_started/downloads.html"; \
	  exit 1; }
	emcc $(WASM_SRCS) $(WASM_FLAGS) -o web/beancraft.mjs
	@echo "Built web/beancraft.mjs (+ web/beancraft.wasm). Serve the repo over HTTP and open /web/index.html"

# QBE runtime library for compiled programs: the bignum implementation, the
# bc_* shims the generated code calls, and the device subsystem.
libbcruntime.a: $(BUILDDIR)/release/qbe_runtime.o $(BUILDDIR)/release/bignum.o $(BUILDDIR)/release/devices.o
	ar rcs $@ $^

$(BUILDDIR)/release/qbe_runtime.o: $(SRCDIR)/qbe_runtime.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# Copy examples from Janet version
examples:
	cp -r ../beancraft/examples/* examples/

.PHONY: examples
