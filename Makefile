# the `board` cli. standalone: it shares brd_format.c with the host app, but
# building it needs nothing from that app's toolchain, no sdk, no ndk, no
# runtime dependency.
#
#   make            build build/board
#   make test       build, then run the format's own round-trip and rejection tests
#   make clean
#
# output goes under build/, never a temp/scratch directory, keep build
# products out of source control and out of anything session-scoped.

CC ?= clang
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wconversion -Wshadow -pedantic
BUILD_DIR := ../Build/Products
BOARD := $(BUILD_DIR)/board

.PHONY: all test clean
all: $(BOARD)

$(BOARD): board.c brd_format.c brd_format.h
	@mkdir -p "$(BUILD_DIR)"
	$(CC) $(CFLAGS) board.c brd_format.c -o "$(BOARD)"

test: $(BOARD)
	./tests/run-tests.sh "$(BOARD)"

clean:
	rm -f "$(BOARD)"
