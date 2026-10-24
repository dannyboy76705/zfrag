# zfrag Makefile
#
# Layout: every source file is in this folder (main.c is the command dispatch, mount.c the FUSE mount,
# built in only if fuse3 is found or FUSE=1). tests/ holds format_check.c (make check) and the *_test.sh
# suites that drive zfrag (make check-all); docs/ the clang build notes.
#
# Usage:
#   make                 build build/zfrag
#   make CC=clang        use clang
#   make SAN=1           address + undefined-behaviour sanitizers
#   make FUSE=0          build without the mount code
#   make check           compile and run tests
#   make install PREFIX=/usr DESTDIR=/tmp/stage

CC         ?= cc
PKG_CONFIG ?= pkg-config
PREFIX     ?= /usr/local
MANDIR     ?= $(PREFIX)/share/man
DESTDIR    ?=
OPT        ?= -O2
FUSE       ?= auto

BUILD     := build

# ---- flags ---------------------------------------------------------
CFLAGS ?= $(OPT) -g
override CFLAGS += -Wall -Wextra -I. -MMD -MP -pthread

# The format header stays strict C99 (tests/format_check.c compiles it that way).
CORE_FLAGS := -std=c99 -pedantic
USER_FLAGS := -std=gnu99 -D_FILE_OFFSET_BITS=64

ifdef SAN
override CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
override LDFLAGS += -fsanitize=address,undefined
endif

# ---- libraries -----------------------------------------------------
ZSTD_CFLAGS := $(shell $(PKG_CONFIG) --cflags libzstd 2>/dev/null)
ZSTD_LIBS   := $(shell $(PKG_CONFIG) --libs libzstd 2>/dev/null || echo -lzstd)
XXH_CFLAGS  := $(shell $(PKG_CONFIG) --cflags libxxhash 2>/dev/null)
XXH_LIBS    := $(shell $(PKG_CONFIG) --libs libxxhash 2>/dev/null || echo -lxxhash)

ifeq ($(FUSE),auto)
  ifeq ($(shell $(PKG_CONFIG) --exists fuse3 2>/dev/null && echo yes),yes)
    FUSE := 1
  else
    FUSE := 0
  endif
endif

ifeq ($(FUSE),1)
  FUSE_CFLAGS := $(shell $(PKG_CONFIG) --cflags fuse3) -DZFRAG_WITH_FUSE -DFUSE_USE_VERSION=31
  FUSE_LIBS   := $(shell $(PKG_CONFIG) --libs fuse3)
endif

# ---- sources and objects -------------------------------------------
# mount.c is built only with FUSE
SRC := $(filter-out mount.c,$(wildcard *.c))
ifeq ($(FUSE),1)
  SRC += mount.c
endif
OBJ := $(SRC:%.c=$(BUILD)/%.o)

TEST_SRC := $(wildcard tests/*_check.c)
TEST_BIN := $(TEST_SRC:tests/%.c=$(BUILD)/tests/%)

.PHONY: all check check-all clean install uninstall help

all: $(BUILD)/zfrag

$(BUILD)/zfrag: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(ZSTD_LIBS) $(XXH_LIBS) $(FUSE_LIBS) -pthread

# mjpegmount.c is also the standalone mjpegmount program; here it is compiled as a library (see mjpegmount.h)
$(BUILD)/mjpegmount.o: override CFLAGS += -DMJPEGMOUNT_AS_LIBRARY

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(USER_FLAGS) $(ZSTD_CFLAGS) $(XXH_CFLAGS) $(FUSE_CFLAGS) -c -o $@ $<

# ---- tests ---------------------------------------------------------
$(BUILD)/tests/%: tests/%.c zfrag_format.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CORE_FLAGS) -Werror -o $@ $< $(LDFLAGS)

check: $(TEST_BIN)
	@test -n "$(TEST_BIN)" || { echo "no tests found in tests/"; exit 1; }
	@set -e; for t in $(TEST_BIN); do echo "== $$t"; $$t; done

# also build zfrag and run the shell tests that drive it (needs libzstd and libxxhash)
check-all: check $(BUILD)/zfrag
	@echo "== tests/pack_test.sh"
	@sh tests/pack_test.sh $(BUILD)/zfrag
	@echo "== tests/packtest_test.sh"
	@sh tests/packtest_test.sh $(BUILD)/zfrag
	@echo "== tests/mount_test.sh"
	@sh tests/mount_test.sh $(BUILD)/zfrag
	@echo "== tests/glue_test.sh"
	@sh tests/glue_test.sh $(BUILD)/zfrag
	@echo "== tests/cdc_test.sh (set CDCSTORE=DIR with the cdcstore programs to run it)"
	@sh tests/cdc_test.sh $(BUILD)/zfrag
	@echo "== tests/mkv_test.sh (set MJPEGSTORE=path to run it)"
	@sh tests/mkv_test.sh $(BUILD)/zfrag
	@echo "== tests/assets_test.sh"
	@sh tests/assets_test.sh $(BUILD)/zfrag
	@echo "== tests/split_test.sh"
	@sh tests/split_test.sh $(BUILD)/zfrag

# ---- housekeeping --------------------------------------------------
install: $(BUILD)/zfrag
	install -D -m 0755 $(BUILD)/zfrag $(DESTDIR)$(PREFIX)/bin/zfrag
	install -D -m 0644 man/zfrag.1 $(DESTDIR)$(MANDIR)/man1/zfrag.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/zfrag $(DESTDIR)$(MANDIR)/man1/zfrag.1

clean:
	rm -rf $(BUILD)

help:
	@sed -n "2,15p" Makefile

-include $(OBJ:.o=.d)
