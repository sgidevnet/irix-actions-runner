# irix-actions-runner

A native GitHub Actions self-hosted runner for SGI IRIX 6.5.22 and later.

GitHub's official runner is .NET and cannot run on IRIX. This is a clean-room
reimplementation of the runner wire protocol in C99. It ships as one n32 binary
needing only `libc.so.1`, `libpthread.so` and `libm.so`, all IRIX base. OpenSSL
is linked statically and the trust roots ship alongside it.

The runtime is split client/server. The server holds the registration and
long-polls GitHub for work. The client executes one job from a message file and
reports it.

- **On an SGI**, `runner run` is both halves in one process. Steps execute on
  the machine.
- **On a Linux host with Docker**, `runner serve` is the server for a pool of
  registered identities. Each job it accepts gets a container holding an
  emulated Indy, which mounts the message at `/job` and runs `runner execjob`
  inside IRIX.

## Getting started on an SGI

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

For a real one, see
[irix-actions-figlet-demo](https://github.com/sgidevnet/irix-actions-figlet-demo):
it builds figlet from upstream and shows what breaks on IRIX.

## Getting started on Linux

Steps still execute on IRIX: each job gets its own emulated Indy in a
container. What you provide is a Linux host with Docker.

**1. Build.** The parser output is committed, so this is the whole toolchain:

```sh
sudo apt-get install -y libssl-dev
make
```

**2. Register a pool.** One registration token registers all of them:

```sh
./runner configure --url https://github.com/OWNER/REPO --token <token> \
    --count 4 --name-prefix irix
```

That writes `irix-0` through `irix-3`, each holding its own `.runner`,
`.credentials` and `.rsakey`, and each named after its directory. The
registration token is not needed again: every identity authenticates with an
RSA key of its own from here on. 64 identities is the ceiling.

**3. Serve.**

```sh
./runner serve --count 4 --name-prefix irix \
    --image ghcr.io/sgidevnet/irix-worker:0.4.3-indy
```

One parent process and one child per identity, each long-polling on its own. A
job a child accepts is written to a staging directory under `$TMPDIR`, mode
0700, and bind mounted at `/job` in a fresh container. The guest restores a
snapshot of a booted IRIX rather than cold booting: 13.92 s mean over 12 trials
against 246.60 s.

Ctrl-C or `SIGTERM` forwards to every child once and then waits for all of
them, so no identity is left holding a pool session. A `serve` that starts
after an unclean exit reaps the containers its predecessor left behind.

**4. Uninstall.**

```sh
./runner remove --token <token> --count 4 --name-prefix irix
```

### What is different under Docker

- **This is not a portability gate.** iris decodes MIPS IV whatever CPU it
  reports, so a build can pass here and fault on real hardware.
- **Every job starts clean**, which inverts the workspace caveat below.
  `actions/checkout` re-clones on every job and nothing incremental survives.
- **Budget 583 MiB and about one core per job.** Ten in parallel finished in
  28 s on an 80-core host.
- **Outbound ping does not work** in a default container. iris opens an
  unprivileged ICMP socket, which needs `net.ipv4.ping_group_range` to cover
  the process GID, and Docker's default is `1 0`. Ping to the emulator's own
  NAT gateway is answered synthetically and still works.

`serve` talks to `/var/run/docker.sock`, or to `DOCKER_HOST` when that names a
`unix://` path; any other form is an error rather than a silent fallback. There
is no compose file and no server image.

## Requirements

| | |
|---|---|
| OS | IRIX 6.5.22 or later |
| CPU | MIPS R4000 or later. The binary is n32, MIPS III |
| `run:` steps | nothing beyond base IRIX |
| `uses:` steps | `git` 2.18+, `zip` and `unzip` |
| Building from source | [SGUG-RSE](https://github.com/sgidevnet/sgug-rse) 0.0.7beta at `/usr/sgug`, for GCC 9.2 and OpenSSL 1.1.1d |
| Linux host instead | Docker, and `gcc` plus `libssl-dev` to build |

Built and tested on IRIX 6.5.30m. Earlier 6.5.x releases are expected to work
and are untested.

The tarball carries `cert.pem`, found automatically when it sits next to the
binary. `SSL_CERT_FILE` overrides it, and SGUG-RSE's own bundle at
`/usr/sgug/etc/pki/tls/cert.pem` is used when neither is present.

Configuration is written beside the binary as `.runner`, `.credentials` and
`.rsakey`, or one set per identity directory under `--count`.

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
| `${{ }}` in `run:`, `with:` and `if:` | yes |
| `env:` blocks | readable as `${{ env.X }}`, not exported to the step |
| JavaScript actions | no |
| Container jobs, Docker actions, service containers | no |

Every row holds in both run modes. Most of what is unsupported fails with a
named error rather than hanging; the handful that pass silently are below.

## Writing workflows

Most workflow YAML works unchanged. `${{ }}` is evaluated in a `run:` body, a
`with:` value and `if:`.

| Expressions | Status |
|---|---|
| Operators, literals, precedence | yes |
| `contains` `startsWith` `endsWith` `format` `join` `toJSON` `fromJSON` `case` | yes |
| `success()` `always()` `failure()` `cancelled()` | yes |
| `github` `job` `runner` `env` `secrets` `inputs` `matrix` `needs` `strategy` `vars` | yes |
| `hashFiles()` | no, fails the step by name |
| `steps` context | no, needs `$GITHUB_OUTPUT`; fails the step by name |
| Case folding above ASCII | no, `'Ü' == 'ü'` is false where GitHub says true |

Six things to write differently. The first four produce no error at all.

| Instead of | Write |
|---|---|
| `$NAME` from an `env:` block | The value in the `run:` text. `env:` is readable as `${{ env.NAME }}` but is not exported to the step's shell |
| `$GITHUB_OUTPUT`, `$GITHUB_ENV`, `::` commands | Nothing. Steps cannot pass values to each other, and no `GITHUB_TOKEN` reaches them |
| `timeout-minutes:` on a step | Nothing. It is parsed and ignored; every step gets 3600 seconds, and a step that hits that cancels the job. Job-level `timeout-minutes` is server-side and works |
| `ref:` on `checkout` | A `run:` step with `git`. `ref:` is read only when `github.sha` is empty, which no real trigger produces |
| A nested `path:` on `checkout` or `download-artifact` | One level. Both `mkdir` exactly once, so `path: all/0` fails |
| `path: out` on `upload-artifact`, expecting GitHub's layout | It is `zip -r out`, so the members are `out/...` where the reference action strips that component and a download lands one level deeper |

Steps run under `-e`, and bash steps under `-o pipefail`, matching GitHub. But
`shell:` only understands `bash` and absolute paths: any other value falls
through to bash, so `shell: python` runs your Python as a shell script.

`.agents/skills/irix-workflows/` is the full matrix, cited to source, as an
[Agent Skill](https://agentskills.io). Copy the directory into your own
repository to give an agent writing workflows there the same rules.

`.agents/` is the neutral path. `.claude/skills/`, `.gemini/skills/`,
`.opencode/skills/` and `.cursor/skills/` are symlinks into it, because each
agent scans its own directory. Copilot and Cursor Rules read a different format
and are not covered.

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

QEMU has no SGI machine model and MAME's Indy emulation does not run 6.5
usefully. [iris](https://github.com/techomancer/iris) does, and is what the
Linux mode uses, but it decodes MIPS IV whatever CPU it reports: it will tell
you a package builds, not that it runs on an R4400.

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

Under `serve` the step runs in an emulated machine inside a container, and the
emulator's network is userspace NAT.

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
