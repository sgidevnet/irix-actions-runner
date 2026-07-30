---
name: irix-workflows
description: What a GitHub Actions workflow can and cannot do on an irix-actions-runner self-hosted runner. Load before writing or editing any file under .github/workflows/ that targets `runs-on: [self-hosted, irix]`, and before debugging a step that failed or silently did nothing on one. Covers the three supported `uses:` actions, the expression subset, the step environment, and the divergences from github.com that produce no error at all.
---

# irix-workflows

A C99 reimplementation of the Actions wire protocol for SGI IRIX. Most workflow
YAML runs unchanged; what follows is what does not. `reference.md` carries the
exhaustive matrix with a `file:line` for every row.

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

Download's `path:` also gets a single `mkdir`, so `path: all/0` fails with
`download-artifact: cannot create ...`.

Fanning several artifacts back in is easier on a hosted runner. `ubuntu-latest`
has the real action, with `pattern:` and `merge-multiple:`.

## Steps cannot pass values to each other

No `$GITHUB_OUTPUT`, `$GITHUB_ENV`, `$GITHUB_PATH` or `$GITHUB_STEP_SUMMARY`,
and `::` workflow commands print literally and do nothing. So:

- `${{ steps.x.outputs.y }}` is a hard error, not an empty string.
- Job outputs, and `${{ needs.x.outputs.y }}` downstream, cannot work.
- Write to a file in the workspace, or do the work in one step.

`env:` blocks read back as `${{ env.NAME }}` at every scope but are never
exported, so `$NAME` in a `run:` body is empty. Put the value in the text.

There is no `$GITHUB_TOKEN`, so a step cannot call the REST API.

## Expressions

`${{ }}` is evaluated in a `run:` body, a `with:` value and `if:`. Available:
`contains` `startsWith` `endsWith` `format` `join` `toJSON` `fromJSON`
`success` `always` `failure` `cancelled`.

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

## The two run modes differ in one way that matters

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
