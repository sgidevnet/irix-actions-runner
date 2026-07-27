# irix-actions-runner

A native GitHub Actions self-hosted runner for SGI IRIX 6.5.22 and later.

GitHub's official runner is .NET and cannot run on IRIX. This is a clean-room
reimplementation of the runner wire protocol in C99. It ships as one n32 binary
linking only `libc.so.1`, `libpthread.so` and `libm.so`, all IRIX base.
OpenSSL is static.

Verified on an Octane2 running IRIX64 6.5.30m.

## Requirements

| | |
|---|---|
| OS | IRIX 6.5.22 or later, MIPS n32 |
| Build | [SGUG-RSE](https://github.com/sgidevnet/sgug-rse) 0.0.7beta at `/usr/sgug`, for GCC 9.2 and OpenSSL 1.1.1d |
| Runtime | nothing beyond base IRIX, plus `git` 2.18+ on `PATH` for `actions/checkout` |

## Build

```sh
/usr/sgug/bin/sgugshell make      # the shipping binary
make check                        # MIPSPro c99 compiles every source file
make test                         # unit tests, also runnable on Linux
```

## Configure and run

```sh
./runner configure --url https://github.com/OWNER/REPO --token <registration token> \
                   --name octane --labels irix,mips,mips-n32
./runner run
```

Registration tokens come from Settings, Actions, Runners, New self-hosted
runner, or from `gh api -X POST repos/OWNER/REPO/actions/runners/registration-token`.
Org runners work the same way with an org URL.

Credentials land in `.runner`, `.credentials` and `.rsakey` beside the binary,
mode 0600.

## Feature matrix

| Feature | Status |
|---|---|
| Registration, repo and org | yes |
| v2 broker long poll and mid-session migration | yes |
| `run:` steps under `bash` and `sh` | yes |
| Per-step status and timing | yes |
| Live step state while the job runs | yes |
| Full job and per-step logs in the UI | yes |
| `actions/checkout` | yes, native, against the `git` binary |
| `actions/upload-artifact`, `actions/download-artifact` | yes, native, via `zip` |
| Secret masking from the job's mask list | yes |
| Job cancellation and prompt shutdown | yes |
| Resource limits on every step | yes |
| `chroot` and uid drop | implemented, applies only when started as root |
| Live log tailing during a step | no |
| JavaScript actions | no, rejected with an explicit error |
| Container jobs, Docker actions, service containers | no |

Unsupported step types fail with a named error rather than hanging.

JavaScript actions are not planned. V8 dropped its MIPS backends in 2023, and
Node sits on libuv, which has no IRIX backend. QuickJS would build here, being
C99 with no JIT, but the cost is a `node:` compatibility layer, and the actions
worth having are the ones implemented natively above. Run JS steps on a Linux
runner and hand the result to IRIX in a separate job.

## Runner identity

The machine runs IRIX 6.5 on a big-endian MIPS R12000. The runner reports Linux
on x86-64, because GitHub's system labels are a fixed vocabulary with no IRIX or
MIPS value and ordinary workflows say `runs-on: [self-hosted, linux, x64]`. A
runner describing itself accurately matches nothing.

| Reported | Value sent | Accurate | Consumed by | Effect |
|---|---|---|---|---|
| system label | `Linux` | no, it is IRIX | `runs-on` matching | Required |
| system label | `X64` | no, it is MIPS n32 | `runs-on` matching | Required |
| user labels | `irix`, `mips`, `mips-n32`, model | yes | `runs-on` matching | Deliberate targeting |
| `osDescription` | `IRIX 6.5 mips` | yes | runner list in the UI | Display only |
| `runnerOS`, `os=` | `Linux` | no | nothing | Not validated |
| `RUNNER_OS` | `Linux` | no, by default | `if:` conditions | Branch selection |

`runnerOS` and `os=` are sent as `Linux` for consistency, not necessity.
`RUNNER_OS` is the only one worth changing, and it is a flag:

```sh
SGUG_RUNNER_OS=IRIX ./runner run
```

Off by default: when `runner.os == 'Linux'` fails, third-party actions usually
fall through to their macOS or Windows branch, which fits IRIX worse.

## Examples

Target the machine with the user labels.

```yaml
jobs:
  build:
    runs-on: [self-hosted, irix]
    steps:
      - uses: actions/checkout@v4
      - run: ./configure && make
```

Cross-build on x86, verify on real hardware. A 400MHz R12000 takes hours on a
large package where a cross-compiler takes minutes, so this shape gives the
Octane the part only it can do.

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

Build natively. Slower, self-contained, closer to how SGUG-RSE packages are
built by hand today.

```yaml
jobs:
  build:
    runs-on: [self-hosted, irix]
    steps:
      - uses: actions/checkout@v4
      - run: rpmbuild -ba package.spec --nocheck
```

There is no IRIX emulator worth substituting for the hardware. QEMU has no SGI
machine model and MAME's Indy emulation does not run 6.5 usefully.

## Confinement

Limits are applied in the child between fork and exec. Measured inside a job on
the Octane.

| Limit | Default | Bounds |
|---|---|---|
| `RLIMIT_CPU` | 3600s | an infinite loop, indistinguishable from a long compile until this fires |
| `RLIMIT_AS` | 1536 MB | a link step driving a 2816 MB machine into swap |
| `RLIMIT_FSIZE` | 4096 MB | a runaway writer filling the disk |
| `RLIMIT_NOFILE` | 512 | descriptor exhaustion |
| `RLIMIT_CORE` | 0 | a crashing compiler dumping more than the workspace holds |
| `prctl(PR_MAXPROCS)` | 96 | a fork bomb. IRIX has no `RLIMIT_NPROC` |
| `prctl(PR_TERMCHILD)` | on | a backgrounded process outliving its job |

This bounds accidents, not adversaries. `chroot` plus an unprivileged uid plus
rlimits is the entire toolbox IRIX provides: no namespaces, no seccomp, no
jails, no network isolation, so a step can reach the local network.

`chroot` and the uid drop apply when the runner is started as root. Started by
an ordinary user, the job log says so rather than implying containment that is
not there.

For untrusted input, use GitHub's fork pull request approval setting under
Settings, Actions, General. No local confinement substitutes for it.

## Why

SGUG-RSE has no way to build and test packages on the hardware it targets.
Every surviving SGI machine is locked out of modern CI.

## License

MIT. See [LICENSE](LICENSE).
