# irix-actions-runner

<project>
A GitHub Actions self-hosted runner for SGI IRIX 6.5.22+, written in C99. The
official runner is .NET and cannot run here, so this is a clean-room
reimplementation of the wire protocol.

Success means an SGI workstation registers, appears online, and runs real
workflow jobs. The audience is SGUG-RSE, which today has no way to build and
test packages on the hardware it targets.

Two compilers must accept every source file: GCC 9.2 from SGUG-RSE, which
produces the shipping binary, and MIPSPro 7.4.4m, which is a compile-only
portability gate. Everything is verified on real hardware, because no usable
IRIX emulator exists.
</project>

<critical_constraints>
These are measured on hardware, not inherited assumptions. The first five
produce runtime crashes or silent corruption rather than compile errors, so
neither compiler will catch you.

1. **Never use `%zu`, `%zd` or `%zx`.** IRIX libc's printf does not consume the
   argument, which corrupts varargs parsing for every specifier after it: a
   following `%s` reads a garbage pointer and segfaults. Cast to
   `unsigned long` and use `%lu`. `make check` greps for this.

2. **Never use `__thread` or `_Thread_local`.** IRIX rld has no
   `__tls_get_addr`. It fails at first use, not at link time, so the binary
   builds and then dies in the field. Use `pthread_key_create`.

3. **`long` is 32 bits under n32.** Formatting an `int64_t` with `%ld` silently
   drops the high word, and the protocol's `messageId` really is 64-bit. Use
   `sgug_i64toa`, which avoids printf length modifiers rather than trusting
   `%lld` on a libc that already mishandles `%z`.

4. **Link `-pthread`, never `-lpthread`, and never both.** See sgug-rse issues
   #12 and #13.

5. **Never handle, block or unblock signals 47 and 48.** libpthread reserves
   them.

6. **`-Isrc` must precede `-I/usr/sgug/include`.** SGUG ships JsonCpp at
   `/usr/sgug/include/json/json.h`, which collides with ours. Reversed,
   MIPSPro pulls in the C++ header and dies on `<cstddef>`.

7. **`vsnprintf(NULL, 0, ...)` returns -1.** Do not use it to size a buffer.

8. **`time_t` is 32-bit in every IRIX ABI** and wraps in 2038. Use `int64_t`
   internally and narrow only at syscall boundaries.

9. **`mode_t` is `unsigned long`.** Format with `%lo`.

10. **Missing from IRIX libc**, supplied by `src/compat/`: `strnlen`, `memmem`,
    `strcasestr`, `asprintf`, `getline`, `setenv`, `unsetenv`, `timegm`,
    `mkdtemp`, `strerror_r`, `getopt_long`, `explicit_bzero`.

11. **Absent syscalls and options:** no `MAP_ANON`, no `O_NOFOLLOW`, no `*at`
    family, no `posix_spawn`, no `RLIMIT_NPROC`. `SO_RCVTIMEO` and
    `SO_SNDTIMEO` are rejected with `EPROTONOSUPPORT`, so socket read deadlines
    must be built from `poll(2)`. Without that a stalled peer blocks forever.

12. **`getaddrinfo` is a weak symbol.** `netdb.h` marks it `#pragma optional`,
    which GCC ignores, so a null check folds to constant true. Use
    `gethostbyname`, IPv4 only.

13. **The binary must stay MIPS III.** `-mips3` is set deliberately in the
    Makefile. MIPS IV would exclude the R4400 and R5000 machines this project
    exists for. The release workflow asserts it.
</critical_constraints>

<runner_limitations>
What this runner does *not* implement. Read this before writing or editing any
workflow YAML in `.github/workflows/`, because several of these fail silently.

- **No expression evaluator.** `${{ }}` in a `run:` body arrives as a
  TemplateToken type 3 and is now rejected with an explicit error. In a `with:`
  value it still reads as absent, so `name: x-${{ github.sha }}` on
  upload-artifact silently uploads an artifact called `artifact`. **Every
  `with:` value must be a YAML literal.** Use `$GITHUB_*` shell variables in
  `run:` bodies instead.
- **`env:` blocks are discarded** at workflow, job and step level. Nothing can
  be injected into a job. Bake values into the `run:` text.
- **No `GITHUB_TOKEN` in the step environment**, and no `GITHUB_ENV`,
  `GITHUB_OUTPUT`, `GITHUB_PATH` or `GITHUB_STEP_SUMMARY`. Steps cannot call
  the REST API or pass values to one another. `::` workflow commands are not
  parsed.
- **`if:` is three `strstr` calls** (`src/exec/step.c`). Only bare `always()`,
  `failure()`, `cancelled()` and `success()` behave. Anything else is treated
  as `success()`, and `if: !cancelled()` is inverted.
- **`timeout-minutes:` on a step is parsed and ignored.** Every step gets 3600
  seconds of wall clock, not configurable from YAML. Job-level
  `timeout-minutes` is server-side and does work. Split long work into separate
  steps.
- **Three `uses:` handlers only:** `actions/checkout`,
  `actions/upload-artifact`, `actions/download-artifact`. The version spec is
  ignored. Anything else fails loudly.
- **The workspace is never wiped** between jobs, and `checkout` does not remove
  untracked files. Run `git clean -xdff` when a clean tree matters.
- **Beware SIGPIPE under `pipefail`.** `cmd | head -1` and `cmd | grep -m1`
  make the producer die of SIGPIPE, which fails the step. Use `sed -n 1p`.
</runner_limitations>

<architecture>
```
src/compat/   IRIX libc gaps and platform quirks. Everything depends on this.
src/net/      tcp.c tls.c http.c    sockets, OpenSSL, HTTP/1.1
src/crypto/   rsa.c jwt.c aes.c b64.c
src/json/     vendored parser, case-insensitive key lookup
src/proto/    oauth register listener report results mask config
src/exec/     token job step handlers runjob
src/sandbox/  jail.c confine.c
```

Flow of a job:

```
listener.c   long poll -> BrokerMigration -> broker poll -> RunnerJobRequest
             -> acknowledge -> acquirejob
job.c        parse the message; two serialisations in one payload
runjob.c     build a scrubbed environment, resolve the workspace, loop steps
step.c       fork, confine, exec a shell on a script file, stream output
handlers.c   or dispatch a `uses:` step to a native handler
report.c     live step state and log upload via results.c
             -> completejob, which is what releases the parallelism slot
```

Public symbols take a `sgug_` prefix. `sgug::` is reserved for any C++ added
later. `docs/protocol.md` is the reference for anything on the wire; it records
where github.com diverges from the published documentation, which is often.
</architecture>

<workflows>
## Build and test locally

```sh
make test         # builds everything and runs every test binary; works on Linux
```

Do this first for any change. It is the fast loop, and the tests are portable
C99 so they catch most logic errors without touching hardware.

## Verify on hardware

The test machine is an SGI Octane2 at `mach@10.0.1.161`, IRIX64 6.5.30m. Source
lives in `~/iar`; a configured runner lives in `~/ar2`.

IRIX lacks tools you will reach for by reflex. There is no `pkill` and no
`setsid`, and `rsync` is an unrelated RCS utility, not the file-sync one. Sync
sources with tar over ssh:

```sh
tar cf - src Makefile | ssh mach@10.0.1.161 'cd ~/iar && tar xf -'
ssh mach@10.0.1.161 '/usr/sgug/bin/sgugshell make -C ~/iar'
ssh mach@10.0.1.161 '/usr/sgug/bin/sgugshell make -C ~/iar check'
ssh mach@10.0.1.161 '/usr/sgug/bin/sgugshell make -C ~/iar test'
```

`sgugshell` unsets `CC`, `CFLAGS`, `LDFLAGS` and friends on entry, so put build
variables in `~/.sgug_bashrc`, not `~/.profile`. Non-interactively it execs its
first argument with the rest as arguments. Use `su` for the stock SGI
environment.

To exercise the runner end to end, copy the binary into `~/ar2` and restart it.
A running binary cannot be overwritten in place, so kill it first:

```sh
ssh mach@10.0.1.161 'kill $(ps -e | grep "[r]unner" | awk "{print \$1}"); \
    cp ~/iar/build/runner ~/ar2/runner; \
    cd ~/ar2 && nohup ./runner run > run.log 2>&1 &'
gh workflow run irix-dev.yml --ref <branch>
```

## Cut a release

Semantic versioning. The version lives in `SGUG_PROJECT_VERSION` in
`src/version.h` and in a `CHANGELOG.md` section, and the release workflow
refuses to publish unless the tag agrees with both. `SGUG_RUNNER_VERSION` is a
different thing: the protocol floor GitHub enforces. Do not conflate them.

1. Bump `SGUG_PROJECT_VERSION`, move the `Unreleased` changelog entries into a
   dated section, commit.
2. `git tag vX.Y.Z && git push --tags`.
3. `.github/workflows/release.yml` builds on hardware, asserts the ISA and the
   library dependencies, proves TLS works against the bundle it ships, and
   opens a draft release.
4. Review the tarball, then publish the draft.
</workflows>

<code_style>
Match the surrounding code. Kernel-style C: tabs, 80 columns, declarations at
the top of a block, `goto out` for cleanup.

**Comment only where intent is not evident from the code.** Never restate the
line below. A comment earns its place by explaining *why*, or by recording a
fact about the platform that the reader cannot see.

<example name="a comment worth having">
```c
/*
 * Enum numbers, not names. A name fails the service's decoder, which then
 * zeroes the request and answers 404 on the run id. Values are non-sequential.
 */
static int
status_value(sgug_step_status s)
```
The reader cannot deduce any of this from the code, and the 404 is actively
misleading without it.
</example>

<example name="a comment that is noise">
```c
/* Loop over the steps and run each one. */
for (i = 0; i < job->nsteps; i++)
	run_step(&job->steps[i]);
```
It restates the code. Delete it.
</example>

<example name="narration, which reads as AI slop">
```c
/*
 * This function is responsible for handling the parsing of the job message.
 * It carefully validates each field and gracefully handles any errors that
 * may occur during processing, ensuring robustness.
 */
```
Says nothing checkable. Delete it, or replace it with the one non-obvious fact
about the parser that a reader would otherwise have to discover.
</example>

Other rules:

- No em dashes, anywhere, including documentation and commit messages.
- Public symbols take a `sgug_` prefix. Statics do not.
- Errors return -1 and fill a caller-supplied `char *err, size_t errlen`. Do not
  print from a library function; the caller decides where a message goes.
- Prefer one real function over two with different contracts. `sgug_snprintf`
  exists because two return conventions for the same name caused a live bug.
</code_style>

<verification>
Never claim a behaviour works without having run it. This project has repeatedly
been bitten by plausible-sounding assumptions about IRIX and about the Actions
protocol that turned out to be false, and the failures are usually silent.

Before opening a pull request:

1. `make test` passes on Linux.
2. `make`, `make check` and `make test` all pass on the hardware. `make check`
   is the MIPSPro gate and catches GNU extensions that GCC accepts.
3. If the change touches job execution, run a real workflow against the test
   runner and read the job log in the GitHub UI, not just the runner's stdout.

When reporting results, say what you actually observed. If a step was skipped,
say so. If something is inferred from reading source rather than observed on
the wire, say that too.
</verification>

<scope_discipline>
Keep changes minimal and focused on what was asked.

- **Scope:** do not add features, refactor code, or make improvements beyond
  the request. A bug fix does not need the surrounding code cleaned up.
- **Documentation:** do not add comments or docstrings to code you did not
  change.
- **Defensive coding:** do not add error handling for states that cannot occur.
  Trust internal invariants. Validate at system boundaries: the network, the
  job message, user input.
- **Abstractions:** do not create a helper for a one-time operation, and do not
  design for hypothetical future requirements.

Extracting code into a new file is justified when it makes something testable
that was not, as `src/proto/mask.c` did. It is not justified for tidiness.

Clean up any temporary scripts or scratch files you create.
</scope_discipline>

<conventions>
Commits: lowercase, imperative, terse. `ci:`, `chore:`, `fix:`, `refactor:`
prefixes where they fit.

`[n/m]` numbers the commits **within a single feature**, so a feature split
into three logical commits is `[1/3]`, `[2/3]`, `[3/3]`. It is not a running
counter across the project. A standalone commit carries no counter.

The author is David Stancu. Never add a Claude co-author trailer, a
"Generated with" footer, or any other tool attribution, to a commit message or
a pull request body.

A human reviews every pull request. Write the body for that reader: what
changed, why, and what was verified. State what you did not test.
</conventions>
