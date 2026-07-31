---
name: irix-workflows
description: >-
  Support matrix and authoring rules for GitHub Actions workflows targeting an
  irix-actions-runner self-hosted runner. Load before writing or editing files
  under .github/workflows/ that use `runs-on: [self-hosted, irix]`, and before
  debugging a step that failed or silently did nothing on one. Covers the three
  supported `uses:` actions, expressions, the step environment, and silent
  divergences from github.com.
---

# irix-workflows

Most workflow YAML runs unchanged. This file lists the exceptions that matter
when writing one. Read [reference.md](reference.md) for exact action inputs,
expressions, contexts, environment variables, shell behaviour, limits and step
semantics. It cites the implementation or repository record for each claim.

## Three `uses:` handlers, and nothing else

`actions/checkout`, `actions/upload-artifact`, `actions/download-artifact`.
Matching is on the name alone, so `@v3`, `@v4` and a full SHA all reach the
same handler, while a fork of the action does not. Anything else fails the step:

```
<action> has no native handler. This runner has no JavaScript engine, so only
these are supported: actions/checkout, actions/upload-artifact,
actions/download-artifact
```

`continue-on-error:` covers a non-zero exit and nothing else: not this, not a
bad `if:`, not a step that could not start.

### checkout

Honoured: `repository`, `token`, `fetch-depth`, `path`, `persist-credentials`.

- `ref:` is read **only when `github.sha` is empty**, which no real trigger
  produces. Checking out a different branch does not work. Use a `run:` step.
- `submodules`, `lfs` and `clean` are never read. Submodules stay
  uninitialised, LFS pointers stay pointers, nothing is cleaned.
- `path:` gets a single `mkdir`, so `path: a/b` fails unless `a` already exists.

### upload-artifact and download-artifact

`name` and `path`, nothing else. No `pattern`, no `merge-multiple`, no
exclusions, no `if-no-files-found`, no multi-line `path:`.

**The archive is shaped differently from github.com.** Upload runs `zip -r`
against the path you gave, so `path: out` produces members named `out/hello`
where the reference action strips that component. A download with no `path:`
therefore lands at `out/hello`, and `path: all` lands at `all/out/hello`.

In a workflow spanning both runner types, the artifact's shape depends on which
runner packed it. Always name `path:` on the download.

Download's `path:` also gets a single `mkdir`, so `path: all/0` fails with
`download-artifact: cannot create ...`.

Fan several artifacts back in on a hosted runner when possible. The real action
supports `pattern:` and `merge-multiple:`.

## Steps cannot pass values to each other

No `$GITHUB_OUTPUT`, `$GITHUB_ENV`, `$GITHUB_PATH` or `$GITHUB_STEP_SUMMARY`,
and `::` workflow commands print literally and do nothing. So:

- `${{ steps.x.outputs.y }}` is a hard error, not an empty string.
- Job outputs, and `${{ needs.x.outputs.y }}` downstream, cannot work.
- Write to a file in the workspace, or do the work in one step.

`env:` blocks read back as `${{ env.NAME }}` at every scope but are never
exported, so `$NAME` in a `run:` body is empty. Put the value in the text.

There is no `$GITHUB_TOKEN`, so a step cannot call the REST API.

`PATH` is fixed at
`/usr/sgug/bin:/usr/sgug/sbin:/usr/bin:/bin:/usr/sbin:/usr/bsd`. Nekoware and
Freeware are not on it, so a step reaching for `/usr/nekoware/bin/gcc` has to
extend `PATH` itself, and has to do it again in every step that needs it. The
emulated worker image puts `gcc` and `curl` there, where a full SGUG-RSE install
puts `gcc` on the fixed path already. Nekoware `curl` also needs
`--cacert /usr/sgug/etc/pki/tls/certs/ca-bundle.crt` for HTTPS.

## Expressions

`${{ }}` is evaluated in step `name:`, `if:`, `run:`, `shell:`,
`working-directory:` and every `with:` value. Available: `contains`
`startsWith` `endsWith` `format` `join` `toJSON` `fromJSON` `success` `always`
`failure` `cancelled`, plus the local, nonportable `case` function.

- A bad expression in `name:` leaves the literal name. It does not fail the
  job.
- `hashFiles()` parses and then fails when the step runs, which kills the usual
  `actions/cache` key idiom.
- **An unknown function is a parse error even on a branch that short circuits.**
  `${{ false && hashFiles('x') }}` fails.
- No arithmetic at all: `-` is a legal identifier character, so `1-2` cannot be
  subtraction.
- `success()` and `failure()` are job-scoped. The flag is sticky from the first
  failing step and never resets.
- Case folding is ASCII only, so `'Ü' == 'ü'` is false where github.com says
  true.

`github` `inputs` `job` `matrix` `needs` `strategy` `vars` arrive from the
service and work, matrix jobs included. `runner`, `secrets` and `env` are built
locally. `steps` does not exist. An unknown context is an error; an unknown key
inside a known context is empty.

## Shell

`shell:` takes `bash` or an absolute path. **Any other value silently runs
bash**, so `shell: python` executes your Python as a shell script. `{0}`
templating is not parsed.

bash steps run `--noprofile --norc -e -o pipefail`, anything else just `-e`.

`pipefail` makes SIGPIPE fatal, so `cmd | head -1` and `cmd | grep -m1` fail
the step when the producer dies. Use `sed -n 1p`.

## Targeting a runner

`runs-on: [self-hosted, irix]` reaches any of them. Every runner carries `irix`,
`mips` and `mips-n32`; a pool registered with `configure --count` also carries
`emulated`, and an operator can add more with `configure --labels`.

`runs-on:` is an AND over labels with no negation, so `emulated` pins a job to
the Docker pool while nothing excludes it. Keeping a job off it needs a label
those other runners carry and the pool does not.

An organisation runner can also be withheld by its runner group, in which case
it shows Online and the job queues forever. That is an operator setting, not
something a workflow can express.

`emulated` selects an Indy running under iris. It is not an ISA portability
gate: iris executes MIPS IV instructions regardless of the emulated CPU. A
build can pass there and fault on an R4000 or R5000. Verify portability on real
hardware.

Do not use outbound `ping` as a reachability check under `runner serve`. It
depends on the Docker host's `net.ipv4.ping_group_range`; the NAT gateway may
still answer when the internet does not.

## Identity, timeouts and limits

`RUNNER_OS` is `Linux` and `RUNNER_ARCH` is `X64`. Both are lies: GitHub's
system labels have no IRIX or MIPS value, and a runner describing itself
accurately would match no `runs-on`. Target the `irix` label, never
`runner.os`.

`timeout-minutes:` on a step is parsed and ignored. Every step gets 3600
seconds of wall clock and, separately, 3600 seconds of CPU. A step that hits
the wall clock **cancels the job** rather than failing that one step. Job-level
`timeout-minutes` is server-side and does work, so split long work into
separate steps.

Per step: 1536 MB of address space, 4096 MB maximum file size, 512 open files,
96 processes, no core dumps. Anything a step backgrounds is killed when it
exits.

## The workspace

On an SGI the workspace is never wiped, so untracked files survive between jobs
and `checkout` will not remove them. Run `git clean -xdff` when a clean tree
matters.

Under `runner serve` each job gets a fresh container, so nothing survives and
`checkout` re-clones every time.

Write for the dirty case. It is correct under both.

## Missing from the guest

IRIX 6.5 has no `uuidgen` and its `date` has no `+%s`. `hinv` is at
`/sbin/hinv`. `/dev/urandom` works, but a restored snapshot resumes its entropy
pool with it, so every guest in a `serve` pool returns the same bytes: it
identifies the snapshot, not the machine.
