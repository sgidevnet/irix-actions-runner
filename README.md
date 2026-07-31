# irix-actions-runner

[![ci](https://img.shields.io/github/actions/workflow/status/sgidevnet/irix-actions-runner/ci.yml?label=ci)](https://github.com/sgidevnet/irix-actions-runner/actions/workflows/ci.yml)

A native GitHub Actions self-hosted runner for SGI IRIX 6.5.22 and later. It
runs workflow steps on a real SGI workstation, or in a pool of emulated SGI
Indys served by a Linux host.

GitHub's runner is written in .NET and cannot run on IRIX. This one is a
clean-room implementation of the runner protocol in C99. The IRIX release is a
single n32 MIPS III binary with OpenSSL linked statically.

[irix-actions-figlet-demo](https://github.com/sgidevnet/irix-actions-figlet-demo)
builds figlet from upstream on IRIX. It also includes a
[10-job run on 10 emulated Indys](https://github.com/sgidevnet/irix-actions-figlet-demo/actions/runs/30564803634)
that completed in 32 seconds.

## Getting started

Choose where the jobs will run:

| Mode | Host requirements | Start command |
|---|---|---|
| Real SGI | IRIX 6.5.22 or later, R4000 or later | `runner run` |
| Emulated pool | Linux x86_64 or arm64, glibc 2.39+, OpenSSL 3, Docker | `runner serve` |

The released IRIX binary is n32 and MIPS III. It is built and tested on IRIX
6.5.30m. Earlier 6.5.x releases are expected to work, but have not been tested.

The runner itself needs no SGUG-RSE installation. A workflow still needs the
programs it calls. The native `checkout` handler needs Git 2.18 or later, and
the artifact handlers need `zip` and `unzip`.

### Run on an SGI

Download the IRIX tarball from the
[releases page](https://github.com/sgidevnet/irix-actions-runner/releases).
Stock IRIX `tar` cannot decompress it, so unpack it through `gzip`:

```sh
gzip -dc irix-actions-runner-*-irix6.5-mips-n32.tar.gz | tar xvf -
cd irix-actions-runner-*
```

Check TLS, the certificate bundle, HTTP and the system clock:

```sh
./runner selftest
```

Do this before registering. A wrong clock and an expired CA bundle both cause
confusing failures later. The tarball includes `cert.pem`, which the runner
finds when it sits beside the binary. `SSL_CERT_FILE` overrides it.

Get a registration token from Settings, Actions, Runners, New self-hosted
runner in the repository. From another machine with `gh`, the same token is:

```sh
gh api -X POST repos/OWNER/REPO/actions/runners/registration-token --jq .token
```

Register once, then leave the runner running:

```sh
./runner configure --url https://github.com/OWNER/REPO --token TOKEN
./runner run
```

The runner now appears as Online in the repository's Actions settings.
Configuration is stored beside the binary in `.runner`, `.credentials` and
`.rsakey`.

### Run a pool of emulated Indys

Download the Linux tarball for your host from the
[releases page](https://github.com/sgidevnet/irix-actions-runner/releases),
then unpack it:

```sh
tar xzf irix-actions-runner-*-linux-*.tar.gz
cd irix-actions-runner-*
./runner selftest
```

`selftest` checks the Docker socket as well as TLS and HTTP. `serve` needs read
and write access to `/var/run/docker.sock`, which usually means the account
must be in the `docker` group.

Get a registration token as above, then register one identity per concurrent
job. This example creates four:

```sh
./runner configure --url https://github.com/OWNER/REPO --token TOKEN \
    --count 4 --name-prefix irix
```

Start the pool:

```sh
./runner serve --count 4 --name-prefix irix \
    --image ghcr.io/sgidevnet/irix-worker:0.4.3-indy
```

Always pass `--image`. The built-in default is `irix-worker:latest`, which
Docker looks for on Docker Hub, where it does not exist.

`--count 4` creates `irix-0` through `irix-3`. Each directory contains an
independent runner identity and work directory. One registration token covers
the whole pool. The token is not needed again after registration because each
identity authenticates with its own RSA key. The maximum pool size is 64.

Each accepted job gets a fresh container and a restored IRIX guest. The
published worker image is ready to use. To build a worker image around your
own IRIX disk, see [`worker/README.md`](worker/README.md).

### Organisation runners

Use the organisation URL and an organisation registration token:

```sh
gh api -X POST orgs/ORG/actions/runners/registration-token --jq .token

./runner configure --url https://github.com/ORG --token TOKEN \
    --runnergroup IRIX
```

Add `--count` and `--name-prefix` to the configure command for an emulated
pool. `--runnergroup` defaults to `Default`.

A wrong runner group fails quietly: the runner registers and shows Online, but
GitHub never sends it work because the group cannot access the repository.
Check the group's repository access in the organisation's Actions settings.

### Runner labels

Every runner carries `irix`, `mips` and `mips-n32`. A pool created with
`--count` also carries `emulated`:

```yaml
runs-on: [self-hosted, irix, emulated]
```

This selects the emulated pool. If work must run on real hardware, configure
that runner with `--labels hardware` and select it explicitly. `runs-on` has no
way to exclude a label.

GitHub has no IRIX or MIPS system labels, so the runner also reports the fixed
system labels `Linux` and `X64`. These values exist only to make GitHub dispatch
jobs. The `irix`, `mips` and `mips-n32` labels describe the machine.

The `runner.os` context and `RUNNER_OS` environment variable also default to
`Linux`. `SGUG_RUNNER_OS=IRIX ./runner run` changes them, but third-party
actions commonly treat an unknown OS as macOS or Windows. The Linux default is
usually the less harmful branch.

### Run a first workflow

Commit this as `.github/workflows/irix.yml`:

```yaml
name: irix
on: [push, workflow_dispatch]

jobs:
  system:
    runs-on: [self-hosted, irix]
    steps:
      - run: |
          uname -a
          hinv
```

Push the commit and open the job in the GitHub Actions UI. Its log should show
the IRIX release and the SGI hardware inventory.

To unregister, get a fresh registration token and run:

```sh
./runner remove --token TOKEN
```

For an emulated pool, pass the same `--count` and `--name-prefix` used during
configuration.

## Workflow support

GitHub resolves triggers, matrices, dependencies, permissions, concurrency and
job-level conditions before dispatch. This runner receives one resolved job
and implements its steps.

The following work on both real and emulated IRIX:

- `run:` steps under `bash` or `sh`
- `${{ }}` expressions in `run:`, `with:` and `if:`
- `actions/checkout`, implemented against the guest's `git` binary
- `actions/upload-artifact` and `actions/download-artifact`, implemented with
  `zip` and `unzip`
- live step state, live console output and streamed logs
- secret masking, cancellation and resource limits

The expression evaluator passes all 1,015 active cases in GitHub's own
cross-language corpus:

```text
1015 cases, 5 skipped, 51 differing only by unicode case folding
all expr tests passed
```

The 51 cases compare non-ASCII letters. GitHub's .NET evaluator folds all of
Unicode; this runner folds ASCII, so `'Ü' == 'ü'` is false. The corpus and its
driver live under [`test/fixtures/expressions`](test/fixtures/expressions) and
[`test/test_expr.c`](test/test_expr.c).

The following fail the step with a named error:

- `hashFiles()` and the `steps` context
- any `uses:` action other than the three listed above
- JavaScript actions, Docker actions, container jobs and service containers

JavaScript actions are not planned. Modern V8 has no MIPS backend, and libuv
has no IRIX backend. Run those steps on a hosted runner and pass their results
to IRIX as an artifact.

### Silent differences from GitHub's runner

These features do not fail the step, but they do not behave like the official
runner:

| Feature | What happens | What to do |
|---|---|---|
| `env:` | `${{ env.NAME }}` resolves, but `$NAME` is not exported to the shell | Put `${{ env.NAME }}` directly in `run:` |
| Workflow commands and files | `::error::`, `::add-mask::` and `::set-output::` print literally. There is no `$GITHUB_ENV`, `$GITHUB_OUTPUT`, `$GITHUB_PATH`, `$GITHUB_STEP_SUMMARY` or `$GITHUB_TOKEN` | Keep dependent commands in one `run:` step, or exchange ordinary files in the workspace |
| Step `timeout-minutes:` | Parsed and ignored. Every step has a 3,600 second local deadline | Use job-level `timeout-minutes`, which GitHub enforces server-side |
| `checkout` with `ref:` | The handler checks out `github.sha`; `ref` is only used when that value is absent | Check out another revision in a following `git` step |
| Step `id:` | Stored, but unreadable without the `steps` context | Do not depend on step outputs |

### Shells, paths and workspaces

The step environment is built from scratch. Its `PATH` is:

```text
/usr/sgug/bin:/usr/sgug/sbin:/usr/bin:/bin:/usr/sbin:/usr/bsd
```

Nekoware and Freeware are not included. Extend `PATH` inside every step that
needs one of them. An `env:` block will not export it, and a change made by one
step does not carry into the next.

The published emulated guest keeps `gcc` 3.4.6 and `curl` under
`/usr/nekoware/bin`. That `curl` has no bundled trust roots, so HTTPS needs
`--cacert /usr/sgug/etc/pki/tls/certs/ca-bundle.crt`. Git over HTTPS uses the
SGUG bundle and works without this flag.

Steps run with `-e`, and bash steps also use `-o pipefail`. A pipeline such as
`cmd | head -1` can therefore fail when `head` closes the pipe and `cmd` gets
SIGPIPE. Use `sed -n 1p` when you only need the first line.

`shell:` accepts `bash` and absolute interpreter paths. An unknown name falls
back to the default shell, so `shell: python` runs the script as shell input.

On real hardware, the workspace remains between jobs and `checkout` does not
remove untracked files. Run `git clean -xdff` when a clean tree matters. Every
job under `serve` gets a fresh container, so its workspace starts empty.

### Artifact paths

`checkout` and `download-artifact` create the requested `path:` with one
`mkdir`, so a nested path such as `all/0` fails unless its parent already
exists.

`upload-artifact` keeps the leading path component. Uploading `path: out`
stores `out/...`, while GitHub's action stores only the contents of `out`.
When an artifact moves between a hosted runner and this runner, set `path:` on
the download explicitly and check which side created it.

For the complete workflow matrix, including supported inputs and contexts, see
[`.agents/skills/irix-workflows/`](.agents/skills/irix-workflows). It is an
[Agent Skill](https://agentskills.io) with every claim linked to source.

## How jobs run

On a real SGI, one process listens for work and executes it:

```text
GitHub -> runner run -> shell process for each step
```

On a Linux host, `serve` listens with one process per registered identity. Each
job starts a fresh worker container, which restores an emulated Indy and runs
the same runner binary inside IRIX:

```text
GitHub -> runner serve -> worker container -> emulated IRIX -> runner execjob
```

The job message is written to a mode 0700 staging directory under `$TMPDIR`
and mounted into the container at `/job`. It contains the credentials needed
to report the result, so `execjob` needs no local runner registration.

Ctrl-C and `SIGTERM` are forwarded to every listener. The parent waits for all
of them so no identity is left holding a GitHub session. After an unclean exit,
the next `serve` removes containers left by its predecessor.

`DOCKER_HOST` may override `/var/run/docker.sock` with another `unix://`
socket. TCP Docker endpoints are rejected. There is no compose file and no
server image.

### Limits of the emulated pool

- Each job gets a fresh container with 256 MiB of guest RAM and roughly one
  host CPU core.
- iris executes MIPS IV instructions regardless of the CPU it reports. A build
  can pass in emulation and fault on an R4000 or R5000. Real hardware is the
  portability gate.
- Outbound ping does not work with Docker's default
  `net.ipv4.ping_group_range`. The emulator's own NAT gateway still answers.

## Confinement

Every step runs with limits applied before the shell starts:

| Limit | Default | Stops |
|---|---:|---|
| Step wall clock | 3,600 s | A step that stops making progress |
| `--job-timeout` under `serve` | 14,400 s | A wedged worker container |
| `RLIMIT_CPU` | 3,600 s | An infinite loop |
| `RLIMIT_AS` | 1,536 MB | A link step driving the machine into swap |
| `RLIMIT_FSIZE` | 4,096 MB | A runaway writer filling the disk |
| `RLIMIT_NOFILE` | 512 | Descriptor exhaustion |
| `RLIMIT_CORE` | 0 | Core dumps filling the workspace |
| `prctl(PR_MAXPROCS)` | 96 | A fork bomb; IRIX has no `RLIMIT_NPROC` |
| `prctl(PR_TERMCHILD)` | on | A background process outliving its job |

These limits contain accidents, not hostile code. On real IRIX there are no
namespaces, seccomp, jails or network isolation. `chroot` plus a uid drop is
implemented for a runner started as root, but remains untested. A workflow can
reach the local network.

Under `serve`, the job runs inside an emulated machine in a container and uses
the emulator's userspace NAT. For untrusted pull requests, use GitHub's fork
approval setting under Settings, Actions, General. Local confinement is not a
replacement for approval.

## Building from source

On Linux, install a C compiler and the OpenSSL development headers. The parser
output is committed, so the build does not need Bison:

```sh
sudo apt-get install -y gcc libssl-dev
make
make test
```

On IRIX, use [SGUG-RSE](https://github.com/sgidevnet/sgug-rse) 0.0.7beta at
`/usr/sgug`. It supplies GCC 9.2 and OpenSSL 1.1.1d:

```sh
/usr/sgug/bin/sgugshell make
make check
make test
```

`make check` compiles every source file with MIPSPro 7.4.4m. It is a
portability gate, not another runner build. The released IRIX binary is
cross-compiled with clang and LLD against OpenSSL 1.1.1w; the release workflow
also checks its ISA and dynamic library dependencies.

## Command reference

| Command | Purpose |
|---|---|
| `runner configure` | Register and write credentials |
| `runner run` | Listen for jobs and execute them in the current process |
| `runner serve` | Listen with a Linux pool and run each job in a container |
| `runner execjob` | Run one job message; used inside the worker guest |
| `runner status` | Show the configured identity and server details |
| `runner selftest` | Check TLS, certificates, HTTP, clock and Docker |
| `runner remove` | Deregister and delete local configuration |
| `runner version` | Print the runner and protocol versions |
| `runner help` | Print commands and flags |

[`man/runner.1`](man/runner.1) documents every flag, default, range and failure
mode. [`docs/protocol.md`](docs/protocol.md) records the protocol GitHub
actually serves, including where it differs from the published documentation.

## Why

SGUG-RSE had no way to build and test packages on the hardware it targets.
Every surviving SGI machine was locked out of modern CI.

## Authorship

Built by [@mach-kernel](https://github.com/mach-kernel) with Claude Opus 5.

## License

MIT. See [LICENSE](LICENSE).
