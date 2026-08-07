CC ?= cc
VERSION := 1.0.0
CPPFLAGS ?= -Iinclude -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDFLAGS ?= -Wl,-z,relro,-z,now
LDLIBS ?= -lsqlite3

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := build/qtc-linux-x86_64
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
	QTC_BIN=$(abspath $(BIN)) python3 ./tests/serial_latency_test.py

test: unit-test integration-test

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' all unit-test integration-test
	$(MAKE) clean
	$(MAKE) all test

release-check: clean all test sanitize

package: release-check
	VERSION=$(VERSION) BIN=$(BIN) CC='$(CC)' CFLAGS='$(CFLAGS)' LDFLAGS='$(LDFLAGS)' ./packaging/package-release.sh

install: all
	install -Dm755 $(BIN) $(DESTDIR)/usr/local/bin/qtc

clean:
	rm -rf build src/*.o src/*.d

-include $(OBJ:.o=.d)
