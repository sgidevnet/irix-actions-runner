# Worker image

Runs one CI job inside an emulated SGI Indy and exits. This is the guest half
of the `/job` contract in `src/serve/exec.h`; `runner serve` starts one of
these per job, and needs `--image` to point at it.

    ghcr.io/sgidevnet/irix-worker:0.3.0-indy

## Building

These must sit beside the Dockerfile. None is in this repository:

| file | what it is |
|---|---|
| `iris`, `iris-ci` | from [`sgidevnet/iris` v0.1.0-sgidevnet.2](https://github.com/sgidevnet/iris/releases/tag/v0.1.0-sgidevnet.2), or built from that tag with `--release --features chd,lightning,rex-jit` |
| `nvram.bin` | PROM settings, carrying the MAC address and `console=d` |
| `indy.chd` | an IRIX 6.5 Indy disk image, supplied by the operator |
| `indy.chd.diff.chd` | the guest customisations below |
| `saves/golden/`, `saves/.cas/` | the snapshot the entrypoint restores, taken at a logged-in root shell. Matched to `indy.chd.diff.chd`, so retake it whenever that changes. Omit both to fall back to a cold boot |

```
docker build -t irix-worker:latest .
```

`runjob.sh` is installed inside the guest by hand and lives in the disk image.
The copy here is the source of truth for what that file should contain; the
build does not deploy it.

## Preparing the guest

A stock IRIX 6.5 install needs four changes before it can run a job.

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
closure out of an existing SGUG install is 37 MB, against 607 MB for the SGUG
binary repository. Resolve symlink targets explicitly when you do:
`libgcc_s.so.1` and `libbz2.so.1` both point at differently-named files, and
IRIX `tar -h` does not dereference them.

**A CA bundle** at `/usr/local/runner/cert.pem`, beside the runner binary,
which adopts it on its own. Without one a verified request returns nothing
while `curl -k` returns 200, so the failure looks like a broken network rather
than a missing bundle.

**The runner and its job script**, at `/usr/local/runner/`.

## Why it restores instead of cold booting

Measured on one host, same image and same job message: cold boot 251.96 s,
restore 15.77 s, with byte-identical job logs. `restore` itself is under 1.2 s;
most of what remains is a 5.8 s NFS mount.

This needs `sgidevnet/iris` v0.1.0-sgidevnet.2 or later, which carries the L1D
tag fix while `techomancer/iris` #63 to #66 are in review. Before that fix,
restoring a booted guest panicked every time, 6 attempts out of 6 on the
binaries in the published `0.3.0-indy` image, with a page-cache panic:

```
PANIC: pcache_remove_pfdat couldn't find pfd 0x884198a0, pcache 0x92bbfdf0, ...
```

An earlier revision of this file gave that panic as `stack underflow/overflow`
and blamed the NFS mount. Both were wrong. The string does not appear at all,
and a snapshot taken with the mount live restores as cleanly as one taken
without. Also note the guest still answers `echo ALIVE` while it dumps core, so
liveness alone does not detect the failure; `/job` never becoming readable does.

The other symptom that revision described, a guest that goes mute on the serial
console, is real but is not the same bug. It is sequencing: restoring into a
machine whose CPU has never run wedges it permanently while the emulator keeps
burning a core. `entrypoint.sh` waits for the PROM banner before restoring.

If `saves/$SNAPSHOT` is absent from the image, the entrypoint falls back to a
cold boot, so an image built without a snapshot still works.
