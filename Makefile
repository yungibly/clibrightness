CC = clang
CPPFLAGS = -Isrc
CFLAGS = -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -O2
MACOSX_DEPLOYMENT_TARGET ?= 26.0
export MACOSX_DEPLOYMENT_TARGET

.PHONY: all test analyze
all: build/clibrightness

build:
	mkdir -p build

build/clibrightness: src/main.c src/protocol.c src/protocol.h src/controller.c src/controller.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -arch arm64 src/main.c src/protocol.c src/controller.c -framework IOKit -framework CoreFoundation -o $@

build/protocol_test: tests/protocol_test.c src/protocol.c src/protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -g -fsanitize=address,undefined tests/protocol_test.c src/protocol.c -o $@

build/controller_test: tests/controller_test.c src/controller.c src/controller.h src/protocol.c src/protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -g -fsanitize=address,undefined tests/controller_test.c src/controller.c src/protocol.c -o $@

test: build/protocol_test build/controller_test
	./build/protocol_test
	./build/controller_test

analyze:
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(CPPFLAGS) $(CFLAGS) -arch arm64 src/main.c src/protocol.c src/controller.c
