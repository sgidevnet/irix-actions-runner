# irix-actions-runner

A native GitHub Actions self-hosted runner for SGI IRIX 6.5.22 and later.

GitHub's official runner is .NET and cannot run on IRIX. This is a clean-room
reimplementation of the runner wire protocol in C99. It ships as one n32 binary
linking only `libc.so.1`, `libpthread.so` and `libm.so`, all IRIX base. OpenSSL
is static, and the trust roots ship alongside it.

## Requirements

| | |
|---|---|
| OS | IRIX 6.5.22 or later |
| CPU | MIPS R4000 or later. The binary is n32, MIPS III |
| To run `run:` steps | nothing beyond base IRIX |
| To run `uses:` steps | `git` 2.18+, `zip` and `unzip` on the runner |
| To build from source | [SGUG-RSE](https://github.com/sgidevnet/sgug-rse) 0.0.7beta at `/usr/sgug`, for GCC 9.2 and OpenSSL 1.1.1d |

Built and tested on IRIX 6.5.30m. Earlier 6.5.x releases are expected to work
and are untested.

## Install

Download the tarball from the [releases
page](https://github.com/sgidevnet/irix-actions-runner/releases). Stock IRIX
`tar` cannot decompress, so pipe through `gzip`:

```sh
gzip -dc irix-actions-runner-*.tar.gz | tar xvf -
cd irix-actions-runner-*
./runner selftest
```

`selftest` checks TLS, the certificate bundle, HTTP and the clock. Run it first:
an undisciplined clock and an expired CA bundle are the two things that break a
fresh install, and both produce confusing errors later.

The tarball carries `cert.pem`, which the runner finds automatically when it
sits next to the binary. `SSL_CERT_FILE` overrides it, and SGUG-RSE's own bundle
at `/usr/sgug/etc/pki/tls/cert.pem` is used when neither is present.

## Configure and run

```sh
./runner configure --url https://github.com/OWNER/REPO --token <registration token> \
                   --name sgi --labels irix,mips,mips-n32
./runner run
```

Registration tokens come from Settings, Actions, Runners, New self-hosted
runner, or from
`gh api -X POST repos/OWNER/REPO/actions/runners/registration-token`.
Organisation runners work the same way with an organisation URL.

Credentials are written beside the binary as `.runner`, `.credentials` and
`.rsakey`, mode 0600.

## Build from source

```sh
/usr/sgug/bin/sgugshell make      # the shipping binary
make check                        # MIPSPro c99 compiles every source file
make test                         # unit tests, also runnable on Linux
```

## Feature matrix

| Feature | Status |
|---|---|
| Registration, repository and organisation | yes |
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
| `chroot` and uid drop | implemented, applies only when started as root, untested |
| `${{ }}` expressions | no |
| `env:` blocks | no |
| Live log tailing during a step | no |
| JavaScript actions | no, rejected with an explicit error |
| Container jobs, Docker actions, service containers | no |

Anything unsupported fails with a named error rather than hanging or passing
silently.

### Writing workflows for this runner

There is no expression evaluator yet, so `${{ }}` cannot be used in a `run:`
body or a `with:` value. A `run:` body containing one is rejected outright.
Shell environment variables work normally and cover most of what expressions are
used for:

```yaml
- name: Build the tagged version
  run: |
    echo "building $GITHUB_REF_NAME at $GITHUB_SHA"
    make
```

`GITHUB_REPOSITORY`, `GITHUB_REF`, `GITHUB_REF_NAME`, `GITHUB_SHA`,
`GITHUB_RUN_ID`, `GITHUB_ACTOR`, `GITHUB_WORKSPACE`, `RUNNER_TEMP` and the rest
of the usual set are exported. `GITHUB_TOKEN`, `GITHUB_ENV` and `GITHUB_OUTPUT`
are not, so steps cannot call the REST API or pass values to each other.

Steps run under `-e`, and bash steps under `-o pipefail`, matching GitHub. `if:`
understands `always()`, `failure()`, `cancelled()` and `success()`; anything more
complex is treated as `success()`.

JavaScript actions are not planned. V8 dropped its MIPS backends in 2023, and
Node sits on libuv, which has no IRIX backend. QuickJS would build here, being
C99 with no JIT, but the cost is a `node:` compatibility layer, and the actions
worth having are the ones implemented natively above. Run JS steps on a hosted
runner and hand the result to IRIX in a separate job.

## Runner identity

The machine runs IRIX on big-endian MIPS. The runner reports Linux on x86-64,
because GitHub's system labels are a fixed vocabulary with no IRIX or MIPS value
and ordinary workflows say `runs-on: [self-hosted, linux, x64]`. A runner
describing itself accurately matches nothing.

| Reported | Value sent | Accurate | Consumed by | Effect |
|---|---|---|---|---|
| system label | `Linux` | no, it is IRIX | `runs-on` matching | Required |
| system label | `X64` | no, it is MIPS n32 | `runs-on` matching | Required |
| user labels | `irix`, `mips`, `mips-n32`, plus your own | yes | `runs-on` matching | Deliberate targeting |
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

Cross-build on x86, verify on real hardware. Vintage SGI CPUs take hours on a
large package where a cross-compiler takes minutes, so this shape gives the SGI
the part only it can do.

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
`RLIMIT_AS` in particular is worth lowering.

This bounds accidents, not adversaries. `chroot` plus an unprivileged uid plus
rlimits is the entire toolbox IRIX provides: no namespaces, no seccomp, no jails,
no network isolation, so a step can reach the local network.

`chroot` and the uid drop apply when the runner is started as root. Started by an
ordinary user, the job log says so rather than implying containment that is not
there.

For untrusted input, use GitHub's fork pull request approval setting under
Settings, Actions, General. No local confinement substitutes for it.

## Why

SGUG-RSE has no way to build and test packages on the hardware it targets. Every
surviving SGI machine is locked out of modern CI.

## License

MIT. See [LICENSE](LICENSE).
