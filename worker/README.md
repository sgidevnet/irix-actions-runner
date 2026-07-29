# Worker image

Runs one CI job inside an emulated SGI Indy and exits. This is the guest half
of the `/job` contract in `src/serve/exec.h`; `runner serve` starts one of
these per job.

    ghcr.io/sgidevnet/irix-worker:0.3.0-indy

## What is inside

| | |
|---|---|
| `iris`, `iris-ci` | the SGI emulator, built with `chd,lightning,rex-jit` |
| `indy.chd` | an IRIX 6.5 Indy disk image, supplied by the operator |
| `indy.chd.diff.chd` | the guest customisations described below |
| `entrypoint.sh` | boots the guest and hands it the job |

The disk image is **not** in this repository and is not redistributed here.
`docker build` expects it beside the Dockerfile.

## Preparing the guest

A stock IRIX 6.5 install needs four changes before it can run a job. Each was
found the hard way; the reasons are worth keeping.

**A MAC address.** A fresh NVRAM leaves `eaddr` at `00:00:00:00:00:00`, and
IRIX refuses to configure `ec0` without one. Setting it from inside IRIX with
`nvram eaddr` silently does nothing: the variable is PROM-protected. It has to
be set from the command monitor and then persisted:

    >> setenv -f eaddr 08:00:69:12:34:56
    >> setenv -f console d
    $ iris-ci rtc-save

**git, bash, zip and unzip.** IRIX 6.5 ships none of them, and the runner
shells out to all four. They come from SGUG-RSE, whose binaries are `N32
MIPS-III`, which is what an Indy executes. Copying them plus their library
closure out of an existing SGUG install is 37 MB, against 607 MB for the
SGUG binary repository. Resolve symlink targets explicitly when you do:
`libgcc_s.so.1` and `libbz2.so.1` both point at differently-named files, and
IRIX `tar -h` does not dereference them.

**A CA bundle.** OpenSSL 1.0.2 in Nekoware negotiates TLS 1.2 with github.com
happily, so `curl -k` returns 200 while a verified request returns nothing at
all. It is certificate verification that fails, not the protocol.

**The runner and its job script.** `/usr/local/runner/runner` plus
`runjob.sh`, which is what the entrypoint invokes.

## Why it cold boots

`iris-ci restore` takes about 0.6 s, so a snapshot would be the obvious way to
start a job. It does not survive this guest. Restoring a snapshot taken with an
NFS mount live panics IRIX with `stack underflow/overflow`; taken without one,
the guest stops answering on the serial console although the emulator itself
is healthy. Cold boot takes about five minutes, which is small against an IRIX
build, and it has been reliable.
