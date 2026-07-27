# irix-actions-runner

A native GitHub Actions self-hosted runner for SGI IRIX 6.5.22 and later.

GitHub's official runner is .NET, so it cannot run on IRIX. This is a clean-room
reimplementation of the runner wire protocol in C99, built so a 1999 Octane can register
as a real self-hosted runner and execute real workflow jobs.

It ships as one self-contained n32 binary. On a stock IRIX box it needs `libc.so.1`,
`libpthread.so` and `libm.so`, all base OS libraries. OpenSSL is linked statically.

## Status

Early. Phase 0 is complete: verified TLS 1.3 to `api.github.com` with full certificate
validation from an Octane2 running IRIX 6.5.30m, statically linked, under both GCC 9.2 and
MIPSPro 7.4.4m.

Registration and job execution are in progress. This does not run workflows yet.

## Requirements

- IRIX 6.5.22 or later, MIPS n32.
- To build: [SGUG-RSE](https://github.com/sgidevnet/sgug-rse) 0.0.7beta at `/usr/sgug`,
  for GCC 9.2 and OpenSSL 1.1.1d. Only the build needs it; the binary does not.
- To run `actions/checkout`: `git` 2.18 or later on `PATH`.

## Build

```sh
/usr/sgug/bin/sgugshell make
```

Cross-checking against the SGI compiler, if you have it:

```sh
make check      # MIPSPro c99 compiles every source file
make test       # unit tests, also runnable on Linux
```

## What works and what does not

Supported:

- `run:` steps, under `bash` or `sh`.
- `actions/checkout`, implemented natively against the `git` binary.
- Job and step logs, live console output, timeline status, job cancellation.

Not supported, and will fail with an explicit error rather than hanging:

- **JavaScript actions.** Any `uses:` step outside the native handler list is rejected.
  Node is not realistic here: V8 dropped its MIPS backends in 2023, and Node sits on
  libuv, which has no IRIX backend and does not list IRIX even as a best-effort target.
  QuickJS would run on IRIX, being plain C99 with no JIT, but the expensive part is a
  `node:` compatibility layer, and the actions worth having are the ones implemented
  natively here anyway.
- **Container jobs and Docker actions.** There is no container runtime on IRIX.
- **Service containers.**

If your workflow needs a JavaScript action, run that step on a Linux runner and hand the
work to IRIX in a separate job.

## Runner identity

The machine runs IRIX 6.5 on a big-endian MIPS R12000. The runner tells GitHub it is
Linux on x86-64.

It has to. GitHub's system labels are a fixed vocabulary with no IRIX or MIPS value, and
ordinary workflows are written `runs-on: [self-hosted, linux, x64]`. A runner that
described itself accurately would match nothing and never be given a job.

Here is every place an identity is reported, and whether it is honest:

| Reported | Value sent | Accurate | Consumed by | Effect |
|---|---|---|---|---|
| system label | `Linux` | no, it is IRIX | `runs-on` matching | Required. Without it `runs-on: [self-hosted, linux]` never schedules here |
| system label | `X64` | no, it is MIPS n32 | `runs-on` matching | Required for the usual `runs-on: [self-hosted, linux, x64]` |
| user labels | `irix`, `mips`, `mips-n32`, model | yes | `runs-on` matching | Lets a workflow target this machine on purpose |
| `osDescription` | `IRIX 6.5 mips` | yes | runner list in the UI | Display only |
| `runnerOS`, `os=` | `Linux` | no | nothing | Not validated. A job ran reporting `IRIX` and the service neither rejected nor noticed it |
| `RUNNER_OS` | `Linux` | no, by default | `if:` conditions in the workflow | Changes which branches a workflow takes |

The labels have to stay wrong for the runner to be usable. `runnerOS` and `os=` are sent
as `Linux` for consistency rather than necessity: nothing reads them today, and if the
service ever starts checking, a known value is the safer thing to have been sending.
`RUNNER_OS` is the only one you would want to change, and it is a flag.

Target the machine deliberately with the user labels:

```yaml
jobs:
  build:
    runs-on: [self-hosted, irix]
    steps:
      - uses: actions/checkout@v4
      - run: ./configure && make
```

Steps can be told the truth independently of the labels:

```sh
SGUG_RUNNER_OS=IRIX ./runner run
```

Off by default. When `runner.os == 'Linux'` fails, third-party actions usually fall
through to their macOS or Windows branch, which fits IRIX worse than the Linux one.

## Two ways to use it

The Octane is a 400MHz R12000. Building a large package on it takes hours, while
cross-compiling the same thing on x86 takes minutes, so the runner is most
useful when it does the part only real hardware can.

**Verify after a cross-build.** The build runs on a hosted Linux runner, the
Octane receives the artifact and proves it actually works on IRIX.

```yaml
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: ./cross-build.sh          # clang targeting mips-sgi-irix6.5
      - uses: actions/upload-artifact@v4
        with: { name: rpms, path: out/ }

  verify:
    needs: build
    runs-on: [self-hosted, irix]
    steps:
      - uses: actions/download-artifact@v4
        with: { name: rpms }
      - run: ./run-tests.sh
```

**Build natively.** Slower, but self-contained and closer to how SGUG-RSE
packages are built by hand today.

```yaml
jobs:
  build:
    runs-on: [self-hosted, irix]
    steps:
      - uses: actions/checkout@v4
      - run: rpmbuild -ba package.spec --nocheck
```

Both work. The first is what you want for a package set; the second is what you
want for something small, or when the build itself is what you are testing.

There is no IRIX emulator worth using as a substitute. QEMU has no SGI machine
model and MAME's Indy emulation does not run 6.5 usefully, so real hardware is
the only way to know a binary works.

## Confinement

Steps run with resource limits applied between fork and exec. Measured inside a
real job on the Octane:

| Limit | Default | Stops |
|---|---|---|
| `RLIMIT_CPU` | 3600s | an infinite loop, which looks exactly like a long compile until this fires |
| `RLIMIT_AS` | 1536 MB | a link step driving a 2816 MB machine into swap |
| `RLIMIT_FSIZE` | 4096 MB | a runaway writer filling the disk |
| `RLIMIT_NOFILE` | 512 | descriptor exhaustion |
| `prctl(PR_MAXPROCS)` | 96 | a fork bomb. IRIX has no `RLIMIT_NPROC` |
| `RLIMIT_CORE` | 0 | a crashing compiler dumping more than the workspace holds |
| `prctl(PR_TERMCHILD)` | on | a backgrounded process outliving its job and holding the workspace open |

This is aimed at accidents rather than adversaries. A parallel build that forks
without bound and a test that resolves a path wrong are ordinary failures on a
shared workstation, and each is cheap to bound.

For the hostile case, use GitHub's fork pull request approval setting under
Settings, Actions, General. A workflow from a fork should not run here
unreviewed, and no amount of local confinement substitutes for that.

`chroot` and a uid drop are implemented and applied when the runner is started
as root. Started by an ordinary user, which is the usual case, the job log says
so rather than implying containment that is not there.

What none of this stops: IRIX has no network isolation, so a step can reach the
local network. It has no namespaces, no seccomp and no jails. `chroot` plus an
unprivileged uid plus rlimits is the whole toolbox the OS provides.

## Why

SGUG-RSE has no way to build and test packages on the hardware it targets. Every surviving
SGI machine is locked out of modern CI. This fixes that.

## License

MIT. See [LICENSE](LICENSE).
