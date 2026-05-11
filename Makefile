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
OBJS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)

# Exclude main.o for tests
LIB_OBJS = $(filter-out $(BUILDDIR)/main.o,$(OBJS))

TARGET = beancraft

.PHONY: all clean debug test sdl

all: $(BUILDDIR) $(TARGET)

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS = $(DEBUG_LDFLAGS)
debug: $(BUILDDIR) $(TARGET)

sdl: CFLAGS += -DBC_SDL $(SDL_CFLAGS)
sdl: LDLIBS += $(SDL_LIBS)
sdl: $(BUILDDIR) $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

# Test targets
TEST_SRCS = $(wildcard $(TESTDIR)/*.c)
TEST_BINS = $(patsubst $(TESTDIR)/%.c,$(BUILDDIR)/%,$(TEST_SRCS))

test: CFLAGS = $(DEBUG_CFLAGS)
test: LDFLAGS = $(DEBUG_LDFLAGS)
test: $(BUILDDIR) $(LIB_OBJS) $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "Running $$t..."; $$t || exit 1; done

$(BUILDDIR)/test_%: $(TESTDIR)/test_%.c $(LIB_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -I include -o $@ $^

clean:
	rm -rf $(BUILDDIR) $(TARGET) libbcruntime.a

# QBE runtime library for compiled programs: the bignum implementation, the
# bc_* shims the generated code calls, and the device subsystem.
libbcruntime.a: $(BUILDDIR)/qbe_runtime.o $(BUILDDIR)/bignum.o $(BUILDDIR)/devices.o
	ar rcs $@ $^

$(BUILDDIR)/qbe_runtime.o: $(SRCDIR)/qbe_runtime.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Copy examples from Janet version
examples:
	cp -r ../beancraft/examples/* examples/

.PHONY: examples
