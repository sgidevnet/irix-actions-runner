# irix-actions-runner

A GitHub Actions self-hosted runner for SGI IRIX 6.5.22+, written in C99. Clean-room
reimplementation of the runner wire protocol; the official runner is .NET and cannot
run here.

## Build

```sh
make                # GCC 9.2 (SGUG-RSE), static OpenSSL, the shipped binary
make check          # MIPSPro c99 compile of every source file, portability gate
make test           # unit tests, runs on Linux or IRIX
```

`make` needs SGUG-RSE at `/usr/sgug`. `make check` needs MIPSPro 7.4.4m at `/usr/bin/c99`.

## Test machine

SGI Octane2, `mach@10.0.1.161`, IRIX64 6.5.30m, dual R12000 400MHz, 2816MB.

```sh
ssh mach@10.0.1.161            # stock IRIX environment
/usr/sgug/bin/sgugshell        # SGUG-RSE environment (bash 5, GCC 9.2, GNU userland)
/usr/sgug/bin/sgugshell make   # non-interactive: execs $1 with the remaining args
```

`sgugshell` unsets `CC`, `CFLAGS`, `LDFLAGS` and friends on entry. Put build variables in
`~/.sgug_bashrc`, not `~/.profile`. Use `su` for the stock SGI environment.

## Layout

```
src/compat/   IRIX libc gaps and platform quirks. Everything else depends on this.
src/net/      tcp.c tls.c http.c    sockets, OpenSSL, HTTP/1.1
src/crypto/   rsa.c jwt.c aes.c b64.c
src/json/     vendored parser, case-insensitive key lookup
src/proto/    oauth register session listener job report
src/exec/     expand expr step handlers
src/sandbox/  jail.c confine.c
```

Public symbols take a `sgug_` prefix. `sgug::` is reserved for any C++ added later.

## Platform constraints

These are verified on the hardware, not inherited assumptions. Violating the first three
produces runtime crashes, not compile errors.

1. **Never use `%zu`, `%zd` or `%zx`.** IRIX libc's printf does not consume the argument,
   which corrupts varargs parsing for every subsequent specifier; a following `%s` reads a
   garbage pointer and segfaults. Cast to `unsigned long` and use `%lu`. `make check`
   greps for this.
2. **Never use `__thread` or `_Thread_local`.** IRIX rld has no `__tls_get_addr` and fails
   at first use, not at link time. Use `pthread_key_create`.
3. **Link `-pthread`, never `-lpthread`, and never both.** See sgug-rse issues #12 and #13.
4. **Never handle, block or unblock signals 47 and 48.** libpthread reserves them.
5. `vsnprintf(NULL, 0, ...)` returns -1. Do not use it to size buffers.
6. `mode_t` is `unsigned long`. Format with `%lo`.
7. No `MAP_ANON`, no `O_NOFOLLOW`, no `*at` family, no `posix_spawn`.
8. `time_t` is 32-bit in every IRIX ABI and wraps in 2038. Use `int64_t` internally and
   narrow only at syscall boundaries.
9. Missing from IRIX libc, supply via `src/compat/`: `strnlen`, `memmem`, `strcasestr`,
   `asprintf`, `getline`, `setenv`, `unsetenv`, `timegm`, `mkdtemp`, `strerror_r`,
   `getopt_long`, `explicit_bzero`.

## Toolchain findings from the phase 0 spike

- Static `libcrypto.a` + `libssl.a` link under GCC and produce a binary needing only
  `libm.so`, `libpthread.so` and `libc.so.1`, all IRIX base. That is the shipping target.
- `-lz` is required; SGUG's OpenSSL is built with zlib.
- `_rld_new_interface` is undefined even inside `libc.so.1` because IRIX resolves it from
  rld at load time. No static link can satisfy it. `src/compat/irix.c` provides a stub;
  only OpenSSL's unused `dlfcn_pathbyaddr` references it.
- MIPSPro's `ld32` dies with an internal error on GCC-produced archives. MIPSPro is a
  compile-only gate; GNU ld does all linking.
- `getaddrinfo` is a weak symbol (`netdb.h` marks it `#pragma optional`, which GCC
  ignores, so a null check compiles to constant true). We use `gethostbyname` and IPv4
  only.

## Clock skew

GitHub's `Date` response header is a coarse skew reference. Two independently
NTP-disciplined machines, the Linux dev host and the Octane, both measure themselves
7 seconds ahead of it, so the offset is server side and not a local error.

Consequently the skew correction must only engage past a threshold of roughly a minute.
Applying every measured offset would inject a spurious 7 second correction on a machine
whose clock is already right. The threshold is safe because the value being protected is
a five minute JWT window with `nbf` already backdated 30 seconds.

The Octane itself is disciplined by `ntpd` as of 2026-07-26; before that it ran `timed`,
a BSD LAN daemon that syncs to peers rather than to an upstream source, and sat 95
minutes slow. Its RTC measures under 1 ppm, so the machine needed a time source, not
drift compensation. Other vintage machines will not all be this lucky, which is why the
correction exists at all.

## Protocol

github.com pushes runners onto the v2 broker flow. A runner that implements only the
classic Azure DevOps protocol registers fine, shows Online, and then silently never
receives a job. Both generations must be handled. See `docs/protocol.md`.

## Conventions

- Comment only where intent is not evident from the code. Never restate the line below.
- No em dashes.
- Commits: lowercase, imperative, terse. `ci:`/`chore:`/`fix:`/`refactor:` prefixes.
  `[n/m]` for a series. Author is David Stancu; do not add Claude co-author trailers.
- A human reviews every PR. Write bodies accordingly.
