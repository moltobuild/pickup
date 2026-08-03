# Bootstrap build for pickup.
# Pickup is built by a Makefile until molto can build itself; the irony is
# deliberate, since pickup is what will let molto's manifest stop naming a
# local compiler. Targets the C23 subset supported by gcc-12 (-std=c2x).

# Force gcc-12 over Make's built-in default (cc), but honor an explicit
# override from the environment or command line (make CC=clang-19 ...).
ifeq ($(origin CC),default)
    CC := gcc-12
endif

STD    ?= c2x
CFLAGS ?= -std=$(STD) -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS ?=

# Record which headers each object was built from, and read those records back
# on the next run. Without this a change to a struct in a header rebuilds only
# the files that happen to be newer, and the objects that were not rebuilt go
# on using the old layout — which does not fail to link, it corrupts memory at
# run time and looks like a bug in the code that was changed.
DEPFLAGS := -MMD -MP

BUILD_DIR := build
BIN       := $(BUILD_DIR)/pickup
TEST_BIN  := $(BUILD_DIR)/pickup_tests

LIB_SRC  := $(shell find src -name '*.c' ! -name 'main.c')
MAIN_SRC := src/main.c
TEST_SRC := $(shell find tests -name '*.c')

# moltest: the test framework, vendored from molto until a registry exists.
MOLTEST_DIR := modules/moltest
MOLTEST_SRC := $(shell find $(MOLTEST_DIR)/src -name '*.c')

LIB_OBJ  := $(LIB_SRC:%.c=$(BUILD_DIR)/%.o)
MAIN_OBJ := $(MAIN_SRC:%.c=$(BUILD_DIR)/%.o)

.PHONY: all build run test clean

all: build

build: $(BIN)

$(BIN): $(LIB_OBJ) $(MAIN_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# The header dependencies written alongside each object. Absent on a clean
# tree, which is why this is -include rather than include.
-include $(LIB_OBJ:.o=.d) $(MAIN_OBJ:.o=.d)

run: build
	./$(BIN) $(ARGS)

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(LIB_OBJ) $(MOLTEST_SRC) $(TEST_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(MOLTEST_DIR)/include $(LIB_OBJ) $(MOLTEST_SRC) $(TEST_SRC) \
	    -o $@ $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)
