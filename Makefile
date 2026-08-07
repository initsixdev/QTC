CC ?= cc
VERSION := 1.0.0
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Darwin hides flock(), LOCK_*, and MSG_DONTWAIT behind _POSIX_C_SOURCE even though
# it provides them, and the Apple linker rejects -Wl,-z,... entirely. Linux flags are
# kept byte-identical to previous releases so recorded build flags do not drift.
ifeq ($(UNAME_S),Darwin)
OS_TAG := macos
CPPFLAGS ?= -Iinclude -D_DARWIN_C_SOURCE
LDFLAGS ?= -Wl,-dead_strip
else
OS_TAG := linux
CPPFLAGS ?= -Iinclude -D_POSIX_C_SOURCE=200809L
LDFLAGS ?= -Wl,-z,relro,-z,now
endif

CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDLIBS ?= -lsqlite3

# Multiplier applied to the latency budgets in tests/serial_latency_test.py.
# Only the sanitize target raises it; see the comment on that target.
QTC_TIMING_SCALE ?= 1

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := build/qtc-$(OS_TAG)-$(UNAME_M)
TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(patsubst tests/%.c,build/tests/%,$(TEST_SRC))
LIB_OBJ := $(filter-out src/main.o src/tui.o src/core.o,$(OBJ))

.PHONY: all clean unit-test integration-test test sanitize release-check package install
all: $(BIN)

build build/tests:
	mkdir -p $@

$(BIN): $(OBJ) | build
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

build/tests/%: tests/%.c $(LIB_OBJ) | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_OBJ) $(LDLIBS)

unit-test: $(TEST_BIN)
	@set -e; for t in $(TEST_BIN); do echo "== $$t =="; "$$t"; done

integration-test: $(BIN)
	QTC_BIN=$(abspath $(BIN)) ./tests/demo_core_test.sh
	QTC_BIN=$(abspath $(BIN)) python3 ./tests/tui_smoke_test.py
	QTC_BIN=$(abspath $(BIN)) QTC_TIMING_SCALE=$(QTC_TIMING_SCALE) python3 ./tests/serial_latency_test.py

test: unit-test integration-test

# The instrumented pass proves correctness, not latency: ASan+UBSan slow the
# SQLite-backed inbox path by roughly twenty times. QTC_TIMING_SCALE relaxes the
# latency budgets for that pass alone; the optimized rebuild below re-runs the
# same tests at the real budgets.
sanitize:
	$(MAKE) clean
	$(MAKE) QTC_TIMING_SCALE=10 CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' all unit-test integration-test
	$(MAKE) clean
	$(MAKE) all test

release-check: clean all test sanitize

package: release-check
ifeq ($(UNAME_S),Darwin)
	@echo "release packaging requires GNU coreutils and runs on Linux only; see BUILDING.md" >&2; exit 1
else
	VERSION=$(VERSION) BIN=$(BIN) CC='$(CC)' CFLAGS='$(CFLAGS)' LDFLAGS='$(LDFLAGS)' ./packaging/package-release.sh
endif

install: all
	mkdir -p $(DESTDIR)/usr/local/bin
	install -m 755 $(BIN) $(DESTDIR)/usr/local/bin/qtc

clean:
	rm -rf build src/*.o src/*.d

-include $(OBJ:.o=.d)
