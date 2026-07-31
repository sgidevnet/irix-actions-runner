# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- `runner serve` now defaults to `ghcr.io/sgidevnet/irix-worker:latest`. Use `--image` to pin a release or run a custom worker.

## [0.4.0-rc1] - 2026-07-31

### Added

- Pooled emulated Indy mode. `runner serve` forks one listener per configured identity and runs each job in a throwaway container holding an emulated SGI Indy, so a Linux host with Docker can serve IRIX jobs without an SGI. `--image` picks the worker image, `--job-timeout` bounds a wedged emulator.
  - `runner configure --count N --name-prefix P` registers N identities as `P-0`, `P-1` and so on, each with its own credentials, work folder and an `emulated` label. `run`, `status` and `remove` take `--dir` or the same `--count`/`--name-prefix` pair.
  - `runner execjob --message FILE` runs one job from a message file with no configured runner present. This is what the guest runs.
  - `--cancel-file PATH` cancels a running job when that file's mtime changes. `SIGINT` and `SIGTERM` cancel rather than kill, so the job still reports a result.
  - `serve` sends `completejob` itself when a container dies without the guest reporting. Nothing renews a job lock, so an unreported job otherwise leaves that identity online at `currentParallelism: 1` and never dispatched to again.
  - The worker image is published as `ghcr.io/sgidevnet/irix-worker`. The IRIX disk image is supplied by the operator and is not redistributed here.
  - `selftest` reports the Docker daemon's version, and why its socket could not be reached when it could not.

- Linux x86_64 and arm64 tarballs. They need glibc 2.39 and OpenSSL 3 from the host and carry no `cert.pem`, because off IRIX the runner reads the distribution's own trust store.

### Changed

- The release cross-compiles every target on Linux and runs each binary's unit tests on hardware of its own architecture before packaging. The IRIX binary now comes from clang and LLD against a cross-built OpenSSL 1.1.1w rather than SGUG's GCC 9.2 and 1.1.1d. No source changed, and `make` on an SGI still uses GCC.

### Fixed

- The job lease was never renewed, so the run service dropped any job past ten minutes and `completejob` answered 404.
- `uname -s` is `IRIX` on a 32-bit kernel and `IRIX64` on a 64-bit one, so an Indy took the Linux branch of the Makefile and failed on the first header.
- `connect` treated any `poll` return other than 1 as a failure, so a signal arriving mid-connect failed the connection.
## [0.3.0] - 2026-07-28

### Added

- An evaluator for the `${{ }}` expression language, in `src/expr/`. It works in a `run:` body, in a `with:` value and in `if:`, over every context the job message carries, including `env` and `secrets`. The grammar is `src/expr/grammar.y`; the generated parser is committed, so no build needs bison.
- `test/fixtures/expressions`, GitHub's own cross-language conformance corpus for the language, vendored and driven by `test/test_expr.c`.

### Fixed

- `${{ }}` in a `with:` value read as absent, so an artifact whose name was built from run metadata silently uploaded as `artifact`.
- `if:` was three `strstr` calls. `if: !cancelled()` evaluated inverted, and anything with a comparison in it was treated as `success()`.
- A `run:` body containing `${{ }}` was rejected outright; it now runs.
- `canceled()` with one `l` was accepted and behaved as `cancelled()`. The reference registers only the two-`l` spelling, so a misspelling now fails the step instead of quietly always reporting "not cancelled".

### Known limitations

- `hashFiles()` and the `steps` context are not implemented. Both fail the step by name rather than reading as empty.
- Case-insensitive comparison folds ASCII only.
- An `env:` block is readable through the `env` context but is still not exported into the step's shell environment.

## [0.2.0] - 2026-07-28

### Added

- Live console output. Step output now appears in the UI while the step is still running, over the WebSocket at `FeedStreamUrl`, which is what the official runner uses. Best effort: a failed feed is abandoned and the uploaded log remains authoritative.
- Logs of any size. Output is streamed in 2 MB blocks to an append blob instead of being held whole and sent at step end, so a long build no longer grows the runner's memory in step with its own output.

### Fixed

- Steps now run under `-e`, and bash steps under `-o pipefail`, matching GitHub's documented defaults. Only the last command in a multi-line `run:` body used to decide the step's result, so any check followed by another command was not a gate.
- A `run:` body containing `${{ }}` is rejected with an explanation instead of producing an empty script that was reported as succeeded.
- Secret redaction no longer writes past its buffer for a mask shorter than three characters.
- Cancellation and the step deadline were only checked when the output poll timed out, so a step printing continuously could not be cancelled and could not time out.
- The whole-job log is accumulated as steps run rather than stitched from every step's buffer at job end, so a job no longer has to hold all of its output at once.
- `docs/protocol.md` claimed the blob API was the live tailing route. It is not; it is the durable log, and the live tail is a separate socket.

### Changed

- Unit tests run on Linux for every pull request.
- The binary is pinned to MIPS III explicitly rather than inheriting it from the compiler default, and the release asserts it.

## [0.1.0] - 2026-07-27

First release. An SGI running IRIX 6.5 registers as a self-hosted runner and executes real workflow jobs: checkout, build, artifacts, and results reported back to GitHub.

### Added

- Runner registration and configuration for a repository or an organisation, persisted to `.runner`, `.credentials` and `.rsakey`
- Listener: OAuth client assertions, agent session, long poll, message decryption, and the v2 broker migration github.com now requires
- Job execution: `run:` steps under bash or sh, per-step status and timing, full job and step logs, secret masking, and job cancellation
- Live step state during a job, through the results service step update API
- Native `actions/checkout` against the `git` binary
- Native `actions/upload-artifact` and `actions/download-artifact`
- Step confinement: CPU, address space, file size and descriptor limits, an IRIX process cap against fork bombs, orphan reaping, and an optional chroot with a uid drop when the runner is started as root
- Self-contained n32 binary needing only `libc.so.1`, `libpthread.so` and `libm.so`, with OpenSSL linked statically
- `runner selftest` for diagnosing TLS, certificate and clock problems
- Release pipeline that builds on SGI hardware and publishes a tarball
- Trust roots shipped alongside the binary, found automatically, so the runner works on a machine with no SGUG-RSE installed
- Build system for GCC 9.2 with a MIPSPro `c99` compile gate, and unit tests that run on both Linux and IRIX

### Fixed

- Steps now run under `-e`, and bash steps under `-o pipefail`, matching GitHub's documented defaults. Previously only the last command in a multi-line `run:` body determined the step's result, so any check followed by another command was not a gate at all.
- A `run:` body containing `${{ }}` is now rejected with an explanation. Previously it produced an empty script that was reported as succeeded.
- Secret redaction no longer writes past its buffer when a mask is shorter than three characters.

### Known limitations

- No expression evaluator, so `${{ }}` is unavailable. Use the `$GITHUB_*` shell variables. Workflow, job and step `env:` blocks are ignored.
- No JavaScript actions, no container jobs, no service containers. Anything outside the three native handlers fails with a clear error.
- Logs appear when a step finishes rather than streaming as it runs.
- `chroot` confinement applies only when the runner is started as root, and is untested.

[Unreleased]: https://github.com/sgidevnet/irix-actions-runner/compare/v0.2.0...HEAD [0.2.0]: https://github.com/sgidevnet/irix-actions-runner/compare/v0.1.0...v0.2.0 [0.1.0]: https://github.com/sgidevnet/irix-actions-runner/releases/tag/v0.1.0
