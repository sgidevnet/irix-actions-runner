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

- **JavaScript actions.** Node.js does not exist for IRIX and cannot: V8 removed its MIPS
  backends, and big-endian MIPS was never supported even before that. Any `uses:` step
  outside the native handler list is rejected.
- **Container jobs and Docker actions.** There is no container runtime on IRIX.
- **Service containers.**

If your workflow needs a JavaScript action, run that step on a Linux runner and hand the
work to IRIX in a separate job.

## Runner identity

The runner reports `RUNNER_OS=Linux` and the system labels `self-hosted`, `Linux`, `X64`.

`X64` is untrue. GitHub's system label vocabulary has no MIPS value, and real workflows are
written `runs-on: [self-hosted, linux, x64]`. Claiming it is what makes ordinary workflow
YAML schedulable here.

The truthful labels are also advertised, so workflows can target IRIX deliberately:

```yaml
jobs:
  build:
    runs-on: [self-hosted, irix]
    steps:
      - uses: actions/checkout@v4
      - run: ./configure && make
```

Available labels: `irix`, `mips`, `mips-n32`, and the machine's model (`octane`, `o2`,
`fuel`, `tezro`).

## Sandboxing

Jobs do not run as the runner. The supervisor stays root and never executes workflow code.
Per job it forks and the child enters a chroot, drops to an unprivileged uid, applies
resource limits, caps process count, and surrenders all IRIX capabilities before exec.

What this stops: escaping the work directory, reading files the job uid cannot read,
exhausting memory or file descriptors, fork bombs, filling the disk, leaving orphan
processes behind.

What this does not stop: reaching the local network, since IRIX has no network isolation;
consuming disk up to `RLIMIT_FSIZE` per file; using the whole machine's CPU beyond the
per-step limit. IRIX has no namespaces, no seccomp and no jails. `chroot` plus capabilities
plus rlimits is the entire toolbox the OS provides, and this uses all of it.

Do not run untrusted third-party workflows on a machine you care about.

## Why

SGUG-RSE has no way to build and test packages on the hardware it targets. Every surviving
SGI machine is locked out of modern CI. This fixes that.

## License

MIT. See [LICENSE](LICENSE).
