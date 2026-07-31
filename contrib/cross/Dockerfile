# Cross toolchain for IRIX 6.5 MIPS n32, targeting Linux x86_64 build hosts.
#
# GCC dropped mips-sgi-irix6.5 in the 4.8 era, so there is no modern GCC cross
# to build. clang emits n32 MIPS-III for the target with no patching; the
# linker is the part that needs work, and unxmaal/mogrix carries an LLD 18 with
# IRIX support along with the header shims and CRT sources that go with it.
#
# The sysroot is IRIX's own headers and lib32, captured off an install by
# mogrix/scripts/create-irix-sysroot.sh. It is not in this repository; pass it
# in as a build context:
#
#   docker build -f contrib/cross/Dockerfile \
#       --build-context sysroot=/path/to/irix-sysroot .

FROM ubuntu:24.04

ARG MOGRIX_SHA=1a42e44
ARG OPENSSL_VERSION=1.1.1w

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y --no-install-recommends \
      clang llvm binutils make perl python3 curl ca-certificates git file xz-utils \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --filter=blob:none https://github.com/unxmaal/mogrix.git /opt/mogrix \
    && git -C /opt/mogrix checkout "$MOGRIX_SHA" \
    && chmod 0755 /opt/mogrix/tools/bin/ld.lld-irix-18

COPY --from=sysroot . /opt/irix-sysroot/

ENV IRIX_SYSROOT=/opt/irix-sysroot \
    SGUG_STAGING=/opt/irix-staging \
    IRIX_CLANG=/usr/bin/clang \
    IRIX_LLD=/opt/mogrix/tools/bin/ld.lld-irix-18

# The header search order irix-cc expects. mogrix keeps two directories called
# dicl-clang-compat; the one under cross/include is the real set and replaces
# IRIX's stdint.h and stddef.h, which otherwise need MIPSPro's __c99 and
# __INTADDR__ builtins.
RUN mkdir -p $SGUG_STAGING/include $SGUG_STAGING/lib32 $SGUG_STAGING/bin \
    && ln -s /opt/mogrix/compat/include/mogrix-compat $SGUG_STAGING/include/mogrix-compat \
    && ln -s /opt/mogrix/cross/include/dicl-clang-compat $SGUG_STAGING/include/dicl-clang-compat \
    && ln -s /opt/mogrix/cross/bin/irix-ld $SGUG_STAGING/bin/irix-ld

# crt1.o and crtn.o carry .MIPS.events.* sections whose R_MIPS_SCN_DISP
# relocations make LLD 18 report an internal error. mogrix avoids them by using
# mips3/fixed/, which not every IRIX install has. These are SGI performance
# tool metadata and nothing in a running program reads them, so the sections
# are marked SHT_NULL in place: sh_type only, no offsets move.
COPY strip-events.py /opt/tools/strip-events.py
RUN mkdir -p $IRIX_SYSROOT/usr/lib32/mips3/fixed \
    && python3 /opt/tools/strip-events.py \
         $IRIX_SYSROOT/usr/lib32/mips3/crt1.o $IRIX_SYSROOT/usr/lib32/mips3/crtn.o \
    && mv $IRIX_SYSROOT/usr/lib32/mips3/crt1.o.scrubbed \
          $IRIX_SYSROOT/usr/lib32/mips3/fixed/crt1.o \
    && mv $IRIX_SYSROOT/usr/lib32/mips3/crtn.o.scrubbed \
          $IRIX_SYSROOT/usr/lib32/mips3/fixed/crtn.o

# irix-ld links six objects into every executable. Four come from mogrix's own
# sources; safe_mem and libsoft_float_stubs have none in the tree and are
# stubbed. safe_mem replaces IRIX libc's word-aligned mem/str routines for C++
# overread, and soft_float_stubs covers 128-bit long double. A C program that
# uses neither reaches neither, and the link fails if that stops being true.
RUN set -eux; \
    CC=/opt/mogrix/cross/bin/irix-cc; \
    $CC -c /opt/mogrix/cross/crt/crtbeginT.S -o $SGUG_STAGING/lib32/crtbeginT.o; \
    $CC -c /opt/mogrix/cross/crt/crtendT.S   -o $SGUG_STAGING/lib32/crtendT.o; \
    cp /opt/mogrix/cross/crt/eh_frame_reg.o  $SGUG_STAGING/lib32/eh_frame_reg.o; \
    echo 'void *__dso_handle = &__dso_handle;' > /tmp/dso.c; \
    $CC -c /tmp/dso.c -o $SGUG_STAGING/lib32/dso_handle.o; \
    echo 'static int placeholder; int mogrix_stub(void) { return placeholder; }' > /tmp/stub.c; \
    $CC -c /tmp/stub.c -o $SGUG_STAGING/lib32/safe_mem.o; \
    $CC -c /tmp/stub.c -o /tmp/stub.o; \
    ar rcs $SGUG_STAGING/lib32/libsoft_float_stubs.a /tmp/stub.o

# LLD cannot read SGUG's gcc-built archives at all: GNU as emits STB_LOCAL
# symbols after sh_info and LLD rejects the object outright. So OpenSSL is
# cross-built here with the same toolchain. no-engine is deliberately absent:
# 1.1.1 leaves dangling ENGINE_* references without it.
RUN set -eux; \
    cd /tmp; \
    curl -sSLo o.tar.gz "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"; \
    tar xzf o.tar.gz; \
    cd "openssl-${OPENSSL_VERSION}"; \
    CC=/opt/mogrix/cross/bin/irix-cc AR=ar RANLIB=ranlib \
      ./Configure irix-mips3-gcc no-asm no-async no-dgram no-afalgeng no-tests \
        no-shared no-zlib --prefix=/opt/irix-openssl; \
    make build_generated; \
    make -j"$(nproc)" libcrypto.a libssl.a; \
    mkdir -p /opt/irix-openssl/lib /opt/irix-openssl; \
    cp libcrypto.a libssl.a /opt/irix-openssl/lib/; \
    cp -r include /opt/irix-openssl/include; \
    cd /tmp && rm -rf "openssl-${OPENSSL_VERSION}" o.tar.gz

ENV IRIX_OPENSSL=/opt/irix-openssl

# dlmalloc bypasses IRIX's brk heap limit and is mogrix's default for
# executables. It has no source in the tree, and the runner has been fine on
# libc malloc since it existed, so the link skips it.
ENV MOGRIX_NO_DLMALLOC=1

# Links the package to the repository, which is what lets that repository's
# GITHUB_TOKEN pull it while the package stays private.
LABEL org.opencontainers.image.source=https://github.com/sgidevnet/irix-actions-runner
