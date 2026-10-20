# Yuseri Stage 0 compiler
CC      ?= clang
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g
SRC     := $(wildcard stage0/src/*.c)
OBJ     := $(patsubst stage0/src/%.c,build/%.o,$(SRC))
BIN     := build/yuseri

all: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(OBJ)

build/%.o: stage0/src/%.c stage0/src/*.h stage0/src/errors.def stage0/rt/yu_rt.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

debug: CFLAGS += -O0 -fsanitize=address,undefined
debug: clean $(BIN)

test: $(BIN)
	sh tests/run_golden.sh
	sh tests/run_errors.sh
	sh tests/run_programs.sh
	sh tests/run_rt.sh
	sh tests/check_errors.sh

check-errors:
	sh tests/check_errors.sh

clean:
	rm -rf build

.PHONY: all debug test check-errors clean
