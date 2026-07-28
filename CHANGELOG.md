# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Live console output. Step output now appears in the UI while the step is
  still running, over the WebSocket at `FeedStreamUrl`, which is what the
  official runner uses. Best effort: a failed feed is abandoned and the
  uploaded log remains authoritative.
- Logs of any size. Output is streamed in 2 MB blocks to an append blob
  instead of being held whole and sent at step end, so a long build no longer
  grows the runner's memory in step with its own output.

### Fixed

- `docs/protocol.md` claimed the blob API was the live tailing route. It is
  not; it is the durable log, and the live tail is a separate socket.

## [0.1.0] - 2026-07-27

First release. An SGI running IRIX 6.5 registers as a self-hosted runner and
executes real workflow jobs: checkout, build, artifacts, and results reported
back to GitHub.

### Added

- Runner registration and configuration for a repository or an organisation,
  persisted to `.runner`, `.credentials` and `.rsakey`
- Listener: OAuth client assertions, agent session, long poll, message
  decryption, and the v2 broker migration github.com now requires
- Job execution: `run:` steps under bash or sh, per-step status and timing,
  full job and step logs, secret masking, and job cancellation
- Live step state during a job, through the results service step update API
- Native `actions/checkout` against the `git` binary
- Native `actions/upload-artifact` and `actions/download-artifact`
- Step confinement: CPU, address space, file size and descriptor limits, an
  IRIX process cap against fork bombs, orphan reaping, and an optional chroot
  with a uid drop when the runner is started as root
- Self-contained n32 binary needing only `libc.so.1`, `libpthread.so` and
  `libm.so`, with OpenSSL linked statically
- `runner selftest` for diagnosing TLS, certificate and clock problems
- Release pipeline that builds on SGI hardware and publishes a tarball
- Trust roots shipped alongside the binary, found automatically, so the runner
  works on a machine with no SGUG-RSE installed
- Build system for GCC 9.2 with a MIPSPro `c99` compile gate, and unit tests
  that run on both Linux and IRIX

### Fixed

- Steps now run under `-e`, and bash steps under `-o pipefail`, matching
  GitHub's documented defaults. Previously only the last command in a
  multi-line `run:` body determined the step's result, so any check followed by
  another command was not a gate at all.
- A `run:` body containing `${{ }}` is now rejected with an explanation.
  Previously it produced an empty script that was reported as succeeded.
- Secret redaction no longer writes past its buffer when a mask is shorter than
  three characters.

### Known limitations

- No expression evaluator, so `${{ }}` is unavailable. Use the `$GITHUB_*`
  shell variables. Workflow, job and step `env:` blocks are ignored.
- No JavaScript actions, no container jobs, no service containers. Anything
  outside the three native handlers fails with a clear error.
- Logs appear when a step finishes rather than streaming as it runs.
- `chroot` confinement applies only when the runner is started as root, and is
  untested.

[Unreleased]: https://github.com/sgidevnet/irix-actions-runner/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/sgidevnet/irix-actions-runner/releases/tag/v0.1.0
