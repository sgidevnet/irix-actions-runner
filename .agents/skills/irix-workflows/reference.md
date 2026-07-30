# Support matrix

Every row cites the source that decides it, relative to the runner repository.
`SKILL.md` is the working summary; this is the thing to check when the summary
does not answer the question.

## `uses:` handlers

The table is `src/exec/handlers.c:397-404`. Lookup is `strcmp` on the action
name (`handlers.c:415`), and the version is split off before lookup
(`src/exec/job.c:101-102`), so `@v3`, `@v4`, `@main` and a full SHA are
identical and a fork of the action matches nothing.

Inputs are expression-evaluated into a compact token before the handler sees
them (`src/exec/runjob.c:198-250`). An input no handler reads is dropped with
no diagnostic anywhere.

### actions/checkout, `handlers.c:89-237`

| input | read at | behaviour |
|---|---|---|
| `repository` | `handlers.c:110` | defaults to `github.repository`, joined to `github.server_url` |
| `token` | `handlers.c:126` | defaults to the job's `github_token`, then `system.github.token`; set as `http.<server>/.extraheader` |
| `fetch-depth` | `handlers.c:130` | default `1`. `0` becomes `--tags` on the sha path, and an invalid `--depth=0` on the ref path |
| `path` | `handlers.c:131` | single `mkdir` at `handlers.c:139`, so a nested path fails |
| `persist-credentials` | `handlers.c:132` | anything but the literal `true` unsets the auth header afterwards |
| `ref` | `handlers.c:119` | **only when `github.sha` is empty** (`handlers.c:201-207`) |

Never read: `submodules`, `lfs`, `clean`, `sparse-checkout`,
`sparse-checkout-cone-mode`, `set-safe-directory`, `github-server-url`,
`fetch-tags`, `show-progress`, `filter`, `ssh-key`, `ssh-known-hosts`,
`ssh-strict`, `ssh-user`. Stated at `handlers.c:85-86`.

Sequence: `git init --quiet`, `remote add origin`, `remote set-url origin`,
optional auth header, `fetch`, `checkout --force FETCH_HEAD`
(`handlers.c:148-218`). Every call is prefixed `git -c http.sslVerify=true`
(`handlers.c:60-62`).

### actions/upload-artifact, `handlers.c:274-329`

Reads `name` (default `artifact`) and `path`. Nothing else:
`handlers.c:271-272` names multiple path patterns, exclusions and the
compression level as unimplemented, and `argv` holds one path
(`handlers.c:294-306`).

The archive is `zip -q -r <zip> <path>` run from the workspace, so the leading
component of `path` is preserved. github.com's action strips the least common
ancestor instead. No shell is involved, so a glob is matched by Info-ZIP, not
by bash.

Upload is the v4 three-call protocol at `ARTIFACT_VERSION 7`
(`src/proto/results.c:19`, `results.c:604-620`).

### actions/download-artifact, `handlers.c:332-395`

Reads `name` and `path` only. `pattern`, `merge-multiple`, `run-id`,
`github-token` and `repository` are unread. Destination gets a single `mkdir`
(`handlers.c:353`). Unpack is `unzip -q -o <zip> -d <dest>`
(`handlers.c:377-385`).

`zip` and `unzip` are resolved over a fixed path list
(`src/exec/step.c:288-290`); absent, the step fails with `%s not found`
(`step.c:320`).

### Unsupported

An action not in the table fails at `runjob.c:450-465`, and
`continue-on-error:` is not consulted on that path. A step whose reference type
is neither `script` nor `repository`, meaning a Docker or container step, is
marked unsupported at `job.c:104-108` and fails at `runjob.c:480` with
`container and docker steps cannot run on IRIX: there is no container runtime`.

## Expression functions

Arity table `src/expr/eval.c:813-824`, dispatch `eval.c:858-1043`.

| function | arity | notes |
|---|---|---|
| `contains` | 2 | array membership, else case-insensitive substring; empty needle is true (`eval.c:865-901`) |
| `startsWith`, `endsWith` | 2 | `eval.c:903-923` |
| `format` | 1..255 | `{N}`, `{{`, `}}`; a format specifier is an error (`eval.c:519-605`) |
| `join` | 1..2 | default separator `,` (`eval.c:925-962`) |
| `toJSON`, `toJson` | 1 | `eval.c:964-977` |
| `fromJSON`, `fromJson` | 1 | `eval.c:979-996` |
| `case` | odd, 3..255 | **not a github.com function**, local extension (`eval.c:1017-1033`) |
| `success` | 0 | `!job_failed && !job_cancelled` (`eval.c:998-1002`) |
| `always` | 0 | constant true |
| `cancelled` | 0 | `eval.c:1007-1010` |
| `failure` | 0 | `job_failed && !job_cancelled` (`eval.c:1011-1015`) |
| `hashFiles` | 1..255 | parses, then fails at runtime: `hashFiles is not implemented on this runner` (`eval.c:1035-1039`) |

`success()` and `failure()` read a job-scoped sticky flag set on the first
failing step (`runjob.c:384`), not per-step outcomes.

An unknown name is rejected during **parsing** (`eval.c:854`, reached from
`src/expr/lex.c:196-197`), so it fails even inside a branch that would short
circuit. This is deliberate; see the comment at `eval.c:826-829`.

Grammar `src/expr/grammar.y:65-107`: `||` `&&` `==` `!=` `<` `>` `<=` `>=` `!`
`.` `[]` `.*` `[*]`, calls and grouping. No arithmetic, because `-` is a legal
identifier character (`lex.c:289-293`, rationale `grammar.y:14-15`). `&&` and
`||` yield an operand rather than a boolean (`eval.c:1245-1258`). Strings are
single-quoted with `''` as the only escape (`lex.c:421-461`).

Truthiness: every object and array is truthy, empty included
(`eval.c:357-367`).

Limits: 21000 characters (`src/expr/parse.h:7`), parse depth 50
(`parse.h:8`), eval depth 512 (`parse.h:16`).

## Contexts

Resolution order is `src/expr/ctx.c:231-256`.

| context | source |
|---|---|
| `github`, `inputs`, `job`, `matrix`, `needs`, `strategy`, `vars` | the job message's `contextData` (`ctx.c:249-254`, `docs/protocol.md:507`) |
| `runner` | synthesised (`ctx.c:62-70`): `os`, `arch`, `name`, `temp`, `tool_cache` and `workspace`. `tool_cache` is the same directory as `temp` |
| `secrets` | the message's `variables` filtered on `isSecret` (`ctx.c:72-91`). Only secrets the workflow text references are sent |
| `env` | `environmentVariables` at workflow then job scope, with the step's own block appended last (`ctx.c:171-229`) |
| `steps` | does not exist |

The service sends `runner` as null and it is rebuilt locally
(`docs/protocol.md:510-511`). Synth is checked before `contextData`, so
`runner` and `secrets` cannot be shadowed.

Key lookup inside a context is case-insensitive (`eval.c:113-136`), folding
ASCII only (`eval.c:92-96`). An unknown context is the error
`unrecognized named-value: %s` (`eval.c:1190`); an unknown key inside a known
context is empty (`lex.c:148-150`).

## Step environment

Built from scratch, not inherited (`src/exec/runjob.c:103-174`). `MAX_ENV` is
64 (`runjob.c:21`).

Fixed: `PATH`, `HOME`, `SHELL`, `LC_ALL=C`, `TERM=dumb`, `LD_LIBRARYN32_PATH`,
`RUNNER_OS`, `RUNNER_ARCH`, `RUNNER_NAME`, `RUNNER_TEMP`, `RUNNER_WORKSPACE`,
`RUNNER_TOOL_CACHE`, `CI=true`, `GITHUB_ACTIONS=true`, `GITHUB_WORKSPACE`,
`GITHUB_JOB`, `GIT_CONFIG_NOSYSTEM=1` (`runjob.c:123-153`).

`RUNNER_TOOL_CACHE` is the same directory as `RUNNER_TEMP` (`runjob.c:145`).
`RUNNER_WORKSPACE` equals `GITHUB_WORKSPACE`, where github.com sets it to the
parent.

Then up to twenty more copied from `contextData.github`, uppercased and
prefixed (`runjob.c:155-170`): `GITHUB_REPOSITORY`, `GITHUB_REPOSITORY_OWNER`,
`GITHUB_REF`, `GITHUB_REF_NAME`, `GITHUB_REF_TYPE`, `GITHUB_SHA`,
`GITHUB_RUN_ID`, `GITHUB_RUN_NUMBER`, `GITHUB_RUN_ATTEMPT`, `GITHUB_ACTOR`,
`GITHUB_TRIGGERING_ACTOR`, `GITHUB_WORKFLOW`, `GITHUB_EVENT_NAME`,
`GITHUB_SERVER_URL`, `GITHUB_API_URL`, `GITHUB_GRAPHQL_URL`,
`GITHUB_BASE_REF`, `GITHUB_HEAD_REF`.

Absent everywhere in `src/`: `GITHUB_ENV`, `GITHUB_OUTPUT`, `GITHUB_PATH`,
`GITHUB_STEP_SUMMARY`, `GITHUB_STATE`, `GITHUB_TOKEN`, `GITHUB_EVENT_PATH`,
`GITHUB_ACTION`, `GITHUB_ACTION_PATH`, `RUNNER_DEBUG`, `ACTIONS_RUNTIME_TOKEN`,
`ACTIONS_RESULTS_URL`, `ACTIONS_CACHE_URL`.

`env:` blocks are never exported. `build_env` reads neither the message's
`environmentVariables` nor `step->env`; both are reachable only through
`${{ env.NAME }}`.

## Workflow commands

Nothing parses them. `on_line` (`runjob.c:42-52`) forwards each line to the
reporter and to stdout without inspecting it, and `drain_lines`
(`step.c:89-110`) only splits on newlines. `set-output`, `add-path`,
`add-mask`, `save-state`, `group`, `error`, `warning`, `notice` and `debug`
appear in no source file.

The only masking is applied at report time from the message's `mask` array,
literals only, capped at 64; regex entries are skipped (`job.c:285-295`).

## Shell

`resolve_shell` (`step.c:34-60`) tries `/usr/sgug/bin/bash`, `/bin/bash`,
`/sbin/sh`, `/bin/sh`.

- A `shell:` containing `/` is returned verbatim with no existence check
  (`step.c:43-44`); a bad path means `_exit(127)` with no other diagnostic.
- `shell: bash` picks the first of the two bash paths that is executable.
- **Every other value falls through to the candidate list** (`step.c:51-58`),
  so `shell: python`, `pwsh`, `cmd` and `bash -e {0}` all run bash.

Flags (`step.c:339-360`), chosen by `strstr(basename, "bash")`:

- bash: `--noprofile --norc -e -o pipefail <script>`
- anything else: `-e <script>`

The script is written to `<temp>/step-<pid>.sh` mode 0700 and unlinked after
(`step.c:70-72`, `step.c:387`). An empty script succeeds as a no-op
(`step.c:371-374`). `{0}` templating is not implemented.

`defaults.run.shell` is resolved server-side into each step's `shell` input
(`job.c:93-95`), so it works exactly as far as the per-step value does.

## Timeouts and limits

| what | value | where |
|---|---|---|
| step wall clock | 3600 s, hard-coded | `runjob.c:366`, enforced `step.c:189-225` |
| `RLIMIT_CPU` | 3600 s | `src/sandbox/confine.c:26` |
| `RLIMIT_AS` | 1536 MB | `confine.c:34` |
| `RLIMIT_FSIZE` | 4096 MB | `confine.c:37` |
| `RLIMIT_NOFILE` | 512 | `confine.c:39` |
| `RLIMIT_CORE` | 0 | `confine.c:108` |
| `prctl(PR_MAXPROCS)` | 96 | `confine.c:43` |
| `prctl(PR_TERMCHILD)` | on | `confine.c:125` |
| steps per job | 128, silently truncated | `src/exec/job.h:16`, `job.c:274-275` |
| output line | 8192 bytes, then split | `step.c:17` |

A step that hits the wall clock emits `step timed out`, is sent `SIGTERM`, and
returns `SGUG_STEP_ABORTED` (`step.c:222`), which `runjob.c:493-495` turns into
`CANCELED` and cancels the job.

`timeout-minutes:` on a step is stored (`job.c:137-138`) and read nowhere.

Child exit codes that are not the step's: 125 confinement failed
(`step.c:169`), 126 `dup2` failed (`step.c:148`), 127 `chdir` or `execve`
failed (`step.c:176`, `step.c:184`). A step killed by a signal reports
`128 + signum` (`step.c:277`).

## Job and step semantics

- **Workspace** is `<work>/<repo-basename>/<repo-basename>`, temp is
  `<work>/_temp` (`runjob.c:284-302`). Nothing removes either; the only
  `unlink` calls in `src/exec/` are the step script and the artifact zips.
- **Starting directory** is `GITHUB_WORKSPACE` (`step.c:383-384`), handlers
  included, except checkout's git calls which run in the checkout target.
- **`working-directory:`** works, relative to the workspace (`job.c:96-98`,
  `step.c:171-177`).
- **`continue-on-error:`** applies only to a non-zero exit (`runjob.c:506-510`).
  It does not cover a step that could not start, an `if:` that fails to
  evaluate, an unsupported action, a Docker step, or a failure to evaluate
  `script`, `shell` or `workingDirectory`.
- **`if:`** arrives unwrapped, defaults to `success()` (`job.c:70-71`), and is
  evaluated per step after the job status flags are set (`runjob.c:384-388`). A
  condition that fails to evaluate fails the step and prints the expression
  error; a false one yields `SKIPPED` (`runjob.c:401`).
- **Job and step outputs are impossible**, because both need `$GITHUB_OUTPUT`.
  `${{ needs.x.outputs.y }}` is null and `${{ steps.x.outputs.y }}` is an
  error.
- **Matrix expansion is server-side.** No `matrix`, `strategy` or `needs`
  identifier exists in `src/`; the values arrive in `contextData`.
- **stdout and stderr are merged** into one stream (`step.c:146-147`), so their
  interleaving is the child's write order.
- **Steps get no stdin**, only `/dev/null` (`step.c:151-160`).
- **Non-zero exit** prints `Process completed with exit code %d.`
  (`runjob.c:503-504`).
- **Job result** is `cancelled ? CANCELED : failed ? FAILED : SUCCEEDED`
  (`runjob.c:516-517`).
- **Ambient git config**: steps get `GIT_CONFIG_NOSYSTEM=1` but keep the
  runner's `HOME`, so `~/.gitconfig` is visible to a `run: git ...` step.
