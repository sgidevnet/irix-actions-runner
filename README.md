# irix-actions-runner

A native GitHub Actions self-hosted runner for SGI IRIX 6.5.22 and later.

GitHub's official runner is .NET and cannot run on IRIX. This is a clean-room
reimplementation of the runner wire protocol in C99. It ships as one n32 binary
needing only `libc.so.1`, `libpthread.so` and `libm.so`, all IRIX base. OpenSSL
is linked statically and the trust roots ship alongside it.

## Getting started

You need an SGI running IRIX 6.5.22 or later on an R4000 or later CPU, and a
GitHub repository you can administer.

**1. Install.** Download the tarball from the
[releases page](https://github.com/sgidevnet/irix-actions-runner/releases).
Stock IRIX `tar` cannot decompress, so pipe it through `gzip`:

```sh
gzip -dc irix-actions-runner-*.tar.gz | tar xvf -
cd irix-actions-runner-*
```

**2. Check the machine is ready.**

```sh
./runner selftest
```

This checks TLS, the certificate bundle, HTTP and the clock, and prints what it
found. Run it before anything else: a wrong clock and an expired CA bundle are
the two things that break a fresh install, and both produce confusing errors
later if you skip this.

**3. Get a registration token.** In your repository go to Settings, Actions,
Runners, New self-hosted runner, and copy the token. Or:

```sh
gh api -X POST repos/OWNER/REPO/actions/runners/registration-token --jq .token
```

**4. Register and start.**

```sh
./runner configure --url https://github.com/OWNER/REPO --token <token>
./runner run
```

The runner appears under Settings, Actions, Runners. Leave it running.

**5. Give it something to do.** Commit this as `.github/workflows/irix.yml`:

```yaml
name: irix
on: [push, workflow_dispatch]

jobs:
  build:
    runs-on: [self-hosted, irix]
    steps:
      - uses: actions/checkout@v4
      - run: |
          uname -a
          make
```

Push it and watch the job run on your SGI.

A worked example that builds a real program from upstream source, including the
portability failure it hits on IRIX, is at
[irix-actions-figlet-demo](https://github.com/sgidevnet/irix-actions-figlet-demo).

## Requirements

| | |
|---|---|
| OS | IRIX 6.5.22 or later |
| CPU | MIPS R4000 or later. The binary is n32, MIPS III |
| `run:` steps | nothing beyond base IRIX |
| `uses:` steps | `git` 2.18+, `zip` and `unzip` |
| Building from source | [SGUG-RSE](https://github.com/sgidevnet/sgug-rse) 0.0.7beta at `/usr/sgug`, for GCC 9.2 and OpenSSL 1.1.1d |

Built and tested on IRIX 6.5.30m. Earlier 6.5.x releases are expected to work
and are untested.

The tarball carries `cert.pem`, found automatically when it sits next to the
binary. `SSL_CERT_FILE` overrides it, and SGUG-RSE's own bundle at
`/usr/sgug/etc/pki/tls/cert.pem` is used when neither is present.

Configuration is written beside the binary as `.runner`, `.credentials` and
`.rsakey`, mode 0600.

## What works

| Feature | Status |
|---|---|
| Registration, repository and organisation | yes |
| v2 broker long poll and mid-session migration | yes |
| `run:` steps under `bash` and `sh` | yes |
| Per-step status and timing | yes |
| Live step state while the job runs | yes |
| Live console output while a step runs | yes |
| Logs of any size, streamed in blocks | yes |
| `actions/checkout` | yes, native, against the `git` binary |
| `actions/upload-artifact`, `actions/download-artifact` | yes, native, via `zip` |
| Secret masking from the job's mask list | yes |
| Job cancellation and prompt shutdown | yes |
| Resource limits on every step | yes |
| `chroot` and uid drop | implemented, root only, untested |
| `${{ }}` expressions | no |
| `env:` blocks | no |
| JavaScript actions | no |
| Container jobs, Docker actions, service containers | no |

Anything unsupported fails with a named error rather than hanging or passing
silently.

## Writing workflows

Most workflow YAML works unchanged. Four differences matter.

| Instead of | Write |
|---|---|
| `${{ github.ref_name }}` in a `run:` body | `$GITHUB_REF_NAME`. The usual `$GITHUB_*` and `$RUNNER_*` variables are exported |
| `${{ }}` in a `with:` value | A literal. An expression there reads as absent, so an artifact name built from run metadata silently becomes `artifact` |
| `env:` blocks | Values written directly into the `run:` text |
| `$GITHUB_OUTPUT`, `$GITHUB_ENV` | Nothing. Steps cannot pass values to each other, and no `GITHUB_TOKEN` reaches them |

`${{ }}` in a `run:` body is rejected with an explicit error rather than
running an empty script. Steps run under `-e`, and bash steps under
`-o pipefail`, matching GitHub. `if:` understands `always()`, `failure()`,
`cancelled()` and `success()`; anything more complex is treated as `success()`.

JavaScript actions are not planned. V8 dropped its MIPS backends in 2023, and
Node sits on libuv, which has no IRIX backend. Run JS steps on a hosted runner
and hand the result to IRIX in a separate job.

## Runner identity

The machine runs IRIX on big-endian MIPS. The runner reports Linux on x86-64,
because GitHub's system labels are a fixed vocabulary with no IRIX or MIPS
value and ordinary workflows say `runs-on: [self-hosted, linux, x64]`. A runner
describing itself accurately matches nothing.

| Reported | Value sent | Accurate | Consumed by | Effect |
|---|---|---|---|---|
| system label | `Linux` | no, it is IRIX | `runs-on` matching | Required |
| system label | `X64` | no, it is MIPS n32 | `runs-on` matching | Required |
| user labels | `irix`, `mips`, `mips-n32`, plus your own | yes | `runs-on` matching | Deliberate targeting |
| `osDescription` | `IRIX 6.5 mips` | yes | runner list in the UI | Display only |
| `runnerOS`, `os=` | `Linux` | no | nothing | Not validated |
| `RUNNER_OS` | `Linux` | no, by default | `if:` conditions | Branch selection |

`RUNNER_OS` is the only one worth changing, and it is a flag:

```sh
SGUG_RUNNER_OS=IRIX ./runner run
```

Off by default: when `runner.os == 'Linux'` fails, third-party actions usually
fall through to their macOS or Windows branch, which fits IRIX worse.

## Cross-build, then verify

Vintage SGI CPUs take hours on a large package where a cross-compiler takes
minutes. This shape gives the SGI only the part that needs real hardware.

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

There is no IRIX emulator worth substituting for the hardware. QEMU has no SGI
machine model and MAME's Indy emulation does not run 6.5 usefully.

## Confinement

Limits are applied in the child between fork and exec.

| Limit | Default | Bounds |
|---|---|---|
| `RLIMIT_CPU` | 3600s | an infinite loop, indistinguishable from a long compile until this fires |
| `RLIMIT_AS` | 1536 MB | a link step driving the machine into swap |
| `RLIMIT_FSIZE` | 4096 MB | a runaway writer filling the disk |
| `RLIMIT_NOFILE` | 512 | descriptor exhaustion |
| `RLIMIT_CORE` | 0 | a crashing compiler dumping more than the workspace holds |
| `prctl(PR_MAXPROCS)` | 96 | a fork bomb. IRIX has no `RLIMIT_NPROC` |
| `prctl(PR_TERMCHILD)` | on | a backgrounded process outliving its job |

The defaults suit a machine with a couple of gigabytes of RAM. On a smaller one,
lower `RLIMIT_AS`.

This bounds accidents, not adversaries. `chroot` plus an unprivileged uid plus
rlimits is the entire toolbox IRIX provides: no namespaces, no seccomp, no
jails, no network isolation, so a step can reach the local network.

For untrusted input, use GitHub's fork pull request approval setting under
Settings, Actions, General. No local confinement substitutes for it.

## Building from source

```sh
/usr/sgug/bin/sgugshell make      # the shipping binary
make check                        # MIPSPro c99 compiles every source file
make test                         # unit tests, also runnable on Linux
```

`docs/protocol.md` records what github.com actually serves, which differs from
the published documentation in several places that cost real time to find.

## Why

SGUG-RSE has no way to build and test packages on the hardware it targets.
Every surviving SGI machine is locked out of modern CI.

## Authorship

Built by [@mach-kernel](https://github.com/mach-kernel) with Claude Opus 5.

## License

MIT. See [LICENSE](LICENSE).
