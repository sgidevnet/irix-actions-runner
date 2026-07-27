UNAME_S := $(shell uname -s)

BUILD   := build
# Everything except main.c, so each test binary can supply its own entry point.
LIB_SRCS  := $(wildcard src/compat/*.c src/net/*.c src/crypto/*.c src/json/*.c src/proto/*.c)
SRCS      := $(LIB_SRCS) src/main.c
LIB_OBJS  := $(LIB_SRCS:%.c=$(BUILD)/%.o)
OBJS      := $(SRCS:%.c=$(BUILD)/%.o)
TEST_SRCS := $(wildcard test/*.c)
TEST_BINS := $(TEST_SRCS:test/%.c=$(BUILD)/%)

WARNS   := -Wall -Wextra -Wno-unused-parameter

ifeq ($(UNAME_S),IRIX64)
SGUG    := /usr/sgug
CC      := $(SGUG)/bin/gcc
# -Isrc must precede the SGUG include dir. SGUG ships JsonCpp at
# /usr/sgug/include/json/json.h, which collides with our own json/json.h; with
# the order reversed, MIPSPro pulls in the C++ header and dies on <cstddef>.
CFLAGS  := -std=c99 -O2 $(WARNS) -mabi=n32 -mabicalls \
           -D_SGI_SOURCE -D_SGI_MP_SOURCE -D_SGI_REENTRANT_FUNCTIONS \
           -Isrc -I$(SGUG)/include
# Static OpenSSL keeps the binary self-contained on machines without SGUG-RSE.
# -lz is required: SGUG builds OpenSSL with zlib. MIPSPro's ld32 dies with an
# internal error on these archives, so GNU ld does all linking.
LIBS    := $(SGUG)/lib32/libssl.a $(SGUG)/lib32/libcrypto.a $(SGUG)/lib32/libz.a -pthread
LDFLAGS := -Wl,-rpath,/usr/lib32
else
# Development and unit-test host. IRIX ships OpenSSL 1.1.1d and that is the API
# the sources target; pinning the compatibility level here keeps one code path
# instead of forking it for whatever version the dev box happens to have.
CC      ?= gcc
CFLAGS  := -std=c99 -O2 -g $(WARNS) -D_GNU_SOURCE -Isrc \
           -DOPENSSL_API_COMPAT=0x10100000L -DOPENSSL_SUPPRESS_DEPRECATED
LIBS    := -lssl -lcrypto -pthread
LDFLAGS :=
endif

.PHONY: all check test clean

all: $(BUILD)/runner

$(BUILD)/runner: $(OBJS)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# Portability gate. MIPSPro is compile-only; it rejects GNU C extensions, so a
# clean run here means the source stays buildable with the stock SGI compiler.
# The %z grep is not style policing: IRIX libc printf does not consume the
# argument, which corrupts varargs parsing and segfaults on a following %s.
check:
	@! grep -rn '%[0-9.]*z[udixX]' src/ test/ \
	  || { echo "error: %z format specifier segfaults on IRIX, see CLAUDE.md"; exit 1; }
	@echo "ok: no %z format specifiers"
ifeq ($(UNAME_S),IRIX64)
	@mkdir -p $(BUILD)/mipspro
	@for f in $(SRCS); do \
	  echo "c99 $$f"; \
	  PATH=/usr/bin:/bin /usr/bin/c99 -n32 -O2 -c \
	    -o $(BUILD)/mipspro/check.o \
	    -D_SGI_SOURCE -D_SGI_MP_SOURCE -D_SGI_REENTRANT_FUNCTIONS \
	    -Isrc -I/usr/sgug/include $$f || exit 1; \
	done
	@rm -rf $(BUILD)/mipspro
	@echo "ok: MIPSPro c99 clean"
else
	@echo "skip: MIPSPro check needs IRIX"
endif

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do $$t || exit 1; done

$(BUILD)/%: test/%.c $(LIB_OBJS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_OBJS) $(LIBS)

clean:
	rm -rf $(BUILD)
