# Bootstrap build for pickup.
# molto builds pickup now, from Project.toml, and pickup is what lets molto's
# manifest name capabilities instead of a local compiler. This Makefile stays
# for the bootstrap: the first pickup has to be compiled by something that does
# not need pickup to find a compiler, which is why it hardcodes gcc-12 and the
# C23 subset (-std=c2x) rather than asking.

# Force gcc-12 over Make's built-in default (cc), but honor an explicit
# override from the environment or command line (make CC=clang-19 ...).
ifeq ($(origin CC),default)
    CC := gcc-12
endif

STD    ?= c2x
# The version comes from the manifest, the way it does under molto: one place
# to change it, and no binary that disagrees with the file it was built from.
VERSION := $(shell sed -n 's/^version = "\(.*\)"/\1/p' Project.toml | head -1)

CFLAGS ?= -std=$(STD) -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Iinclude
# Every object depends on Project.toml (see the compile rule below), because
# this define is the one input to a build that no source file mentions. Without
# it, releasing edits the manifest, nothing looks out of date, and the binary
# keeps answering with the version it was first compiled at -- which is how
# 0.7.1 shipped a `pickup -V` that said 0.7.0.
CFLAGS += -DMOLTO_PKG_VERSION='"$(VERSION)"'

# For a caller that wants to add to the build rather than replace it: -Werror,
# sanitizers, an optimisation level. Setting CFLAGS on the command line wins
# over both lines above and takes the version define with it, and a binary
# built that way answers -V with 0.0.0-unknown. Adding through here keeps
# everything that is not being changed — and, unlike LDFLAGS alone, it reaches
# the compiler, which is where -fsanitize has to land to instrument anything.
EXTRA_CFLAGS ?=
CFLAGS += $(EXTRA_CFLAGS)
LDFLAGS ?=

# Record which headers each object was built from, and read those records back
# on the next run. Without this a change to a struct in a header rebuilds only
# the files that happen to be newer, and the objects that were not rebuilt go
# on using the old layout — which does not fail to link, it corrupts memory at
# run time and looks like a bug in the code that was changed.
DEPFLAGS := -MMD -MP

BUILD_DIR := build
BIN       := $(BUILD_DIR)/pickup

LIB_SRC  := $(shell find src -name '*.c' ! -name 'main.c')
MAIN_SRC := src/main.c

LIB_OBJ  := $(LIB_SRC:%.c=$(BUILD_DIR)/%.o)
MAIN_OBJ := $(MAIN_SRC:%.c=$(BUILD_DIR)/%.o)

.PHONY: all build run test clean

all: build

build: $(BIN)

$(BIN): $(LIB_OBJ) $(MAIN_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c Project.toml
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# The header dependencies written alongside each object. Absent on a clean
# tree, which is why this is -include rather than include.
-include $(LIB_OBJ:.o=.d) $(MAIN_OBJ:.o=.d)

run: build
	./$(BIN) $(ARGS)

# The suite is built by molto.
#
# This file compiles the first pickup because something has to; it does not
# compile the tests, which need moltest — a development dependency molto
# resolves into its shared store and reuses across projects (Project.toml).
# Fetching it here as well would be a second store that nothing else reads.
#
# MOLTO names the molto to use (on PATH by default). It asks pickup for a
# compiler; where there is no pickup yet, name one with C_COMPILER, as CI does.
# TEST_ARGS reaches the suite: `make test TEST_ARGS="-k glob"` runs a few cases.
MOLTO     ?= molto
TEST_ARGS ?=

test:
	$(MOLTO) test $(if $(TEST_ARGS),-- $(TEST_ARGS))

clean:
	rm -rf $(BUILD_DIR)
