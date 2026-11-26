CC = clang
CPPFLAGS = -Isrc
CFLAGS = -std=c17 -O2 -Wall -Wextra -Werror -Wpedantic -fstack-protector-strong
LDLIBS = -framework IOKit -framework CoreFoundation
SOURCES = src/main.c src/smc.c src/policy.c src/config.c src/telemetry.c

.PHONY: all test check clean install uninstall

all: build/fanpilot

build:
	mkdir -p build

build/fanpilot: $(SOURCES) $(wildcard src/*.h) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDLIBS) -o $@
	codesign --force --sign - $@

build/tests: tests/test.c src/smc.c src/policy.c src/config.c $(wildcard src/*.h) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined tests/test.c src/smc.c src/policy.c src/config.c $(LDLIBS) -o $@

build/hardware-tests: tests/hardware.c src/telemetry.c $(wildcard src/*.h) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined tests/hardware.c src/telemetry.c $(LDLIBS) -o $@

test: build/tests build/hardware-tests
	./build/tests
	./build/hardware-tests

check: all test
	$(CC) $(CPPFLAGS) $(CFLAGS) --analyze -Xanalyzer -analyzer-werror -Xanalyzer -analyzer-output=text $(SOURCES)
	./build/fanpilot validate config/fanpilot.plist
	plutil -lint config/fanpilot.plist config/local.fanpilot.plist
	sh -n install.sh uninstall.sh

install: all
	sudo /bin/sh ./install.sh

uninstall:
	sudo /bin/sh ./uninstall.sh

clean:
	rm -rf build
