# Runner protocol

API reference for what github.com actually serves, measured against a live org
and repository in July 2026. Where this contradicts the public reference
material or the open reimplementations, this document is what was observed on
the wire.

Fixture: `test/fixtures/job-message.json`, modelled field for field on a real
captured message, synthetic values throughout.

- [Lifecycle](#lifecycle)
- [Hosts and credentials](#hosts-and-credentials)
- [Registration API](#registration-api)
- [OAuth API](#oauth-api)
- [Session API](#session-api)
- [Message API](#message-api)
- [Run service API](#run-service-api)
- [Results service API](#results-service-api)
- [Job message reference](#job-message-reference)
- [Runner identity](#runner-identity)
- [Secrets](#secrets)
- [Appendix: no JavaScript actions](#appendix-no-javascript-actions)

## Lifecycle

Once, at `configure`:

1. [Register](#post-actionsrunner-registration) the runner. Returns the tenant
   URL, pool id and OAuth endpoint.
2. [Add the agent](#post-_apisdistributedtaskpoolspoolidagents) with a
   generated RSA public key. Persist the private key.

Every `run`:

3. [Mint a JWT and exchange it](#post-authorizationurl) for a bearer token.
   Repeat before each expiry.
4. [Create a session](#post-tenant_apisdistributedtaskpoolspoolidsessions).
5. [Poll for messages](#get-tenant_apisdistributedtaskpoolspoolidmessages).
6. On `BrokerMigration`, [delete the pool session and move to the
   broker](#broker-migration). github.com sends this even when registration
   reported no v2 flow, so it is not optional.
7. On `RunnerJobRequest`, [acknowledge](#post-brokeracknowledge) and
   [acquire the job](#post-run_service_urlacquirejob).

Per job:

8. Parse the [job message](#job-message-reference). Its
   `resources.endpoints[0]` carries the job token and the service addresses.
9. Run the steps. Push
   [live step state](#post-workflowstepupdateserviceworkflowstepsupdate) on
   each transition.
10. Upload [step and job logs](#log-upload).
11. [Complete the job](#post-run_service_urlcompletejob). This is what makes
    the job go green and what releases the parallelism slot.

## Hosts and credentials

Three different tokens, in order of appearance.

| Token | Obtained from | Authorises |
|---|---|---|
| Registration token | GitHub UI or REST API, short-lived | [Registration](#registration-api) only |
| Runner OAuth token | [OAuth exchange](#post-authorizationurl), 300s | [Session](#session-api) and [message](#message-api) APIs |
| Job token | `resources.endpoints[0].authorization.parameters.AccessToken` | All job reporting: [run service](#run-service-api), [results service](#results-service-api) |

Service addresses come from `resources.endpoints[0].data` on the job message.

```
PipelinesServiceUrl   base for _apis/distributedtask/hubs/{planType}/plans/{planId}/...
ResultsServiceUrl     results receiver, step update and artifact services
CacheServerUrl        actions cache
FeedStreamUrl         wss:// live console
```

## Registration API

### `POST /actions/runner-registration`

Host `api.github.com`. Auth `Authorization: RemoteAuth <registration token>`.

```json
{"url": "https://github.com/<org>", "runner_event": "register"}
```

Notes:

- The response carried **no `use_v2_flow` field at all**, and the agent-add
  response carried no `ServerUrlV2`. Both suggest the v1 pool flow. Both are
  misleading, see [Broker migration](#broker-migration).
- `authorizationUrl` came back as
  `https://tokenghub.actions.githubusercontent.com/_apis/oauth2/token/<guid>`,
  not the `vstoken.actions.githubusercontent.com` host the reference names. Use
  the returned value verbatim, do not reconstruct it.
- `requireFipsCryptography` is `"False"`, the string, inside a
  `Dictionary<string,string>`. False selects RS256, true selects PS256. Both
  must be implemented since the value is per-deployment.

### `POST /_apis/distributedtask/pools/{poolId}/agents`

Sends the runner name, labels, version and the RSA public key as a modulus and
exponent pair. Returns the agent id.

## OAuth API

### `POST {authorizationUrl}`

Form encoded.

```
grant_type=client_credentials
client_assertion_type=urn:ietf:params:oauth:client-assertion-type:jwt-bearer
client_assertion=<signed JWT>
```

The jwt-bearer URN is the assertion type, never the grant type.

JWT header carries `alg` and `typ` only. No `kid`, no `x5t`, no `iat`.

**The assertion lifetime is measured as `exp - nbf` and capped at exactly 300
seconds.** Backdating `nbf` by 30s for clock skew while computing `exp` from
`now` yields a 330 second window and is rejected:

```json
{"error": "invalid_client",
 "error_description": "Bearer token lifetime 00:05:30 is greater than the allowed maximum lifetime 00:05:00"}
```

That description is only visible if the error body is printed. Otherwise this
is a bare 400 that looks like bad credentials. Derive `exp` from `nbf`.

## Session API

### `POST {tenant}_apis/distributedtask/pools/{poolId}/sessions`

Body includes the agent, the owner name and `useFipsEncryption: false`.
Returns a session id and an `encryptionKey`.

**The session key is wrapped with RSA-OAEP-SHA256, not SHA-1.** The reference
says the digest follows `useFipsEncryption`, so SHA-1 when false. Against a
live session SHA-1 fails and SHA-256 returns the expected 32 bytes. Keep SHA-1
as a fallback for older GHES. Decrypt into a modulus-sized buffer.

409 means a session for this agent already exists, usually self-inflicted, see
[Session lifecycle](#session-lifecycle).

### `DELETE {tenant}_apis/distributedtask/pools/{poolId}/sessions/{sessionId}`

### Session lifecycle

Two mistakes each cost about a minute of 409 retries on every restart.

1. Migrating to the broker without deleting the pool session first. The orphan
   is addressed by the old endpoint and outlives the process.
2. Letting the shutdown abort check apply to the session delete itself. That
   check is what makes a blocked long poll return promptly on SIGTERM, but it
   cancels the cleanup request too, so the session always leaks. Clear the
   abort check before cleanup.

## Message API

### `GET {tenant}_apis/distributedtask/pools/{poolId}/messages`

Query `sessionId`, `lastMessageId`, `status`, `os`, `architecture`. Long poll,
read timeout must be at least 100 seconds. 204 means poll again immediately.

Message bodies are AES-CBC under the session key. Decrypted bodies carry a
UTF-8 BOM, and PKCS#7 unpadding must be lenient: a block-aligned message
produces a full trailing pad block.

### Broker migration

The service sends a `BrokerMigration` message carrying

```json
{"brokerBaseUrl": "https://broker.actions.githubusercontent.com"}
```

**even when registration reported no v2 flow.** A runner that treats it as an
ordinary message keeps polling the pool, receives nothing but repeated
migration notices, and never receives a job. It looks exactly like an idle
runner.

| | Pool | Broker |
|---|---|---|
| Session create | `POST .../pools/{id}/sessions`, 200 | `POST {broker}/session`, **201** |
| Session delete | `DELETE .../sessions/{sessionId}` | `DELETE {broker}/session?sessionId=`, 204 |
| Poll | `GET .../pools/{id}/messages` | `GET {broker}/message` |
| `messageId` | monotonic int64 | **random 32-bit, often negative** |
| Message delete | `DELETE .../messages/{id}` | none, see acknowledge |
| `encryptionKey` | present, messages AES-CBC | **absent, messages in clear** |
| `api-version` | required in `Accept` | not used |

The broker rejects the api-version media type parameter. Send plain
`Accept: application/json`.

### `POST {broker}/acknowledge`

Only for job references, and only when `should_acknowledge` is set.

## Run service API

The broker delivers a reference, not the job.

```json
{"runner_request_id": "020d3759-...",
 "run_service_url": "https://run-actions-2-azure-eastus.actions.githubusercontent.com/116/",
 "billing_owner_id": "O_...",
 "should_acknowledge": true}
```

The run service takes no api-version.

### `POST {run_service_url}acquirejob`

```json
{"jobMessageId": "<runner_request_id>", "runnerOS": "Linux", "billingOwnerId": "<id>"}
```

Returns the same `AgentJobRequestMessage` the v1 flow delivered. 404, 409 and
422 all mean the job is gone or was won by another runner, and are not worth
retrying.

**Acquiring a job takes a parallelism slot that only completion releases.** A
runner that acquires and then exits leaves the agent at
`currentParallelism: 1`, `maxParallelism: 1`, after which nothing is ever
dispatched to it again. The agent still reports `status: online`, and this does
not appear to time out on its own.

`JobCancellation` for such an abandoned job is redelivered on every poll and is
answered immediately rather than long-polled, so a naive loop issues roughly
fifteen requests a second forever. Measured 415 messages in 30 seconds.

### `POST {run_service_url}renewjob`

The v1 job-request lock renewal does not apply: `requestId` is `0` and
`lockedUntil` is `0001-01-01T00:00:00`, .NET's `DateTime.MinValue`.

### `POST {run_service_url}completejob`

Carries the job conclusion and every step result. This replaces the timeline.

```json
{"planId": "...", "jobId": "...", "conclusion": "succeeded",
 "stepResults": [{"external_id": "...", "number": 1, "name": "...",
                  "status": "completed", "conclusion": "succeeded",
                  "started_at": "...", "completed_at": "...",
                  "completed_log_url": null, "completed_log_lines": 42,
                  "annotations": []}],
 "annotations": [], "environmentUrl": null, "billingOwnerId": "..."}
```

Without this the job runs, succeeds locally, and stays "in progress" forever.

A skipped step must still carry timestamps. Sending `"started_at": ""` returns
400 and the parallelism slot is never released.

### Not the timeline API

Every `PATCH .../timelines/{id}/records`, `POST .../logs` and `POST .../events`
against a broker-delivered plan returns 404, silently, while steps run fine
locally. Nothing appears in the UI.

`MessageUtil.IsRunServiceJob(message.MessageType)` puts the official runner in
results-service-only mode (`Runner.Worker/JobRunner.cs`), where it skips the
timeline entirely. Broker-delivered jobs always take that branch.

## Results service API

Twirp over JSON, relative to `ResultsServiceUrl`. Every call is `POST`, JSON in
and JSON out, with `Authorization: Bearer <job token>` and
`Accept: application/json`.

Contracts are open source: `src/Sdk/WebApi/WebApi/ResultsHttpClient.cs` and
`Contracts.cs` in `actions/runner`, callers in
`src/Runner.Common/{ResultsServer,JobServerQueue}.cs`.

Every request carries the same two backend ids.

```
workflow_run_backend_id      = plan.planId
workflow_job_run_backend_id  = jobId
step_backend_id              = the step's id
```

They are also the two ids in the job token's `scp` claim, as
`Actions.Results:<run>:<job>`. Both spellings agree on live jobs. The claim is
the one `@actions/artifact` reads.

Two service prefixes:

```
twirp/results.services.receiver.Receiver/
twirp/github.actions.results.api.v1.<Service>/
```

### Log upload

Three calls per log, under the `Receiver` prefix.

| Step | Call | Notes |
|---|---|---|
| 1 | `GetStepLogsSignedBlobURL` | ids in, `logs_url` out |
| 2 | `PUT {logs_url}` | `x-ms-blob-type: BlockBlob`, `Content-Type: text/plain`, **no `Authorization`**, the URL is already a signed SAS and a bearer token alongside it is rejected |
| 3 | `CreateStepLogsMetadata` | makes the log appear |

```json
{"workflow_job_run_backend_id": "...", "workflow_run_backend_id": "...",
 "step_backend_id": "...", "uploaded_at": "<timestamp>", "line_count": "42"}
```

`line_count` is a string. Protobuf JSON encodes 64-bit integers that way and a
bare number is rejected by some builds.

The same triple exists as `GetJobLogsSignedBlobURL` / `CreateJobLogsMetadata`,
and for step summaries. **The whole-job log is a separate upload from the
per-step logs**, not a concatenation the service derives.

### `POST .../WorkflowStepUpdateService/WorkflowStepsUpdate`

Live step state. Without it the UI shows nothing until
[`completejob`](#post-run_service_urlcompletejob) lands and every step appears
at once. One call per transition, `change_order` incrementing from 1.

```json
{"workflow_run_backend_id": "...", "workflow_job_run_backend_id": "...",
 "change_order": 1,
 "steps": [{"external_id": "<step id>", "number": 1, "name": "...",
            "status": 3, "conclusion": 0, "started_at": "<timestamp>"}]}
```

`external_id` is the step id from the job message, not a generated one.

`status` and `conclusion` are protobuf enums and go on the wire as **numbers**.
The official runner declares them as plain C# enums with no
`StringEnumConverter`, so `JsonMediaTypeFormatter` emits integers.

| Enum | Values |
|---|---|
| `status` | 0 unknown, 3 in_progress, 5 pending, 6 completed |
| `conclusion` | 0 unknown, 2 success, 3 failure, 4 cancelled, 7 skipped |

The values are non-sequential and cannot be inferred from field order.
`github-act-runner` derives `status` with `iota`, giving 1, 2, 3, which is
wrong.

Sending proto enum names instead of numbers fails the service's decoder, which
zeroes the request and answers

```json
404 {"code": "not_found", "msg": "workflow run not found"}
```

The message names the run id. The run id is fine.

The same 404 also appears legitimately on the **first** update of a job, before
the run is visible to this service. It clears by the next transition, so do not
disable the client on it. The official runner only gives up in the
non-results-service-only branch, which broker-delivered jobs never take.

### `POST .../ArtifactService/CreateArtifact`

```json
{"workflow_run_backend_id": "...", "workflow_job_run_backend_id": "...",
 "name": "hello-irix", "version": 7, "mime_type": "application/zip"}
```

Returns `signed_upload_url`. `mime_type` is **required** despite being a
protobuf wrapper type that reads as optional in the generated client. Omitting
it returns "a valid mime_type is required for this artifact". A
`StringValue` serialises as a bare string in Twirp JSON.

Then `PUT` the zip with `x-ms-blob-type: BlockBlob`, and finalise.

### `POST .../ArtifactService/FinalizeArtifact`

```json
{"workflow_run_backend_id": "...", "workflow_job_run_backend_id": "...",
 "name": "hello-irix", "size": "40960", "hash": "sha256:<hex>"}
```

`size` is a string, same 64-bit rule as `line_count`.

### `POST .../ArtifactService/ListArtifacts`

```json
{"workflow_run_backend_id": "...", "workflow_job_run_backend_id": "...",
 "name_filter": "hello-irix"}
```

**A download must list first.** The signed URL request is scoped to the job
that uploaded the artifact, not to the job asking for it, so a downstream job
asking with its own job id gets "artifact not found" even when the run id
matches. The listing is scoped to the run and reports which job owns each
artifact. Take **both** ids from the listing and use them for
`GetSignedArtifactURL`.

### Live log tailing, not implemented

`FeedStreamUrl` is
`wss://results-receiver.actions.githubusercontent.com/_ws/ingest.sock`. In
run-service mode the official runner streams console output over that socket
(`ResultServer.AppendLiveConsoleFeedAsync`) and carries a full RFC 6455 client
for it.

The blob path is the alternative: request `x-ms-blob-type: AppendBlob` instead
of `BlockBlob`, append as the step produces output, then seal with
`x-ms-blob-sealed`. That reuses the existing HTTP client instead of roughly 250
lines of handshake and client-masked framing.

Neither is implemented. Step logs appear when each step finishes.

## Job message reference

Two different serialisations appear in the same message.

Steps use the verbose TemplateToken form:

```json
{"type": 2, "map": [
  {"Key":   {"type": 0, "lit": "script"},
   "Value": {"type": 0, "file": 1, "line": 21, "col": 14, "lit": "uname -a\n"}}]}
```

`contextData` uses the compact PipelineContextData form, and its values are raw
JSON rather than tagged objects:

```json
{"t": 2, "d": [{"k": "ref", "v": "refs/heads/main"},
               {"k": "check_run_id", "v": 90005817181}]}
```

A parser needs both `type`/`lit`/`map`/`Key`/`Value` and `t`/`s`/`d`/`k`/`v`.
The reference documentation describes only the compact form.

### Expressions arrive unevaluated

`${{ }}` is not resolved by the service. A scalar that is entirely one
expression becomes a `type: 3` BasicExpression token carrying `expr`; an
interpolated scalar becomes the same thing wrapped in a `format()` call over the
literal parts, so `run: echo ${{ github.sha }}` arrives as

```json
{"type": 3, "expr": "format('echo {0}', github.sha)"}
```

`if:` corroborates this: the condition arrives as the raw string `"success()"`,
not as a boolean, so the service defers evaluation of both.

A type 3 token carries no `lit`, so a reader that only understands literals
returns its fallback, which is indistinguishable from an absent key. For a
step's `script` that yielded an empty body, and an empty body was treated as a
no-op and reported as succeeded. The runner now rejects the step instead. The
contexts the message does carry are `github`, `inputs`, `job`, `matrix`,
`needs`, `strategy` and `vars`, plus a top-level `environmentVariables` holding
the workflow and job `env:` blocks; `runner` arrives null and is synthesised
locally.

| Field | Value | Reference says |
|---|---|---|
| `plan.planType` | `"actions"` | `"Build"` |
| `timeline.id` | same GUID as `plan.planId` | distinct |
| `requestId` | `0` | a real request id |
| `lockedUntil` | `0001-01-01T00:00:00` | a deadline |
| `contextData.runner` | `null` | populated |
| `messageType` after acquire | stays `RunnerJobRequest` | changes |

Wire JSON is camelCase while the C# contracts are PascalCase, so match keys
case-insensitively.

`messageId` is genuinely 64-bit. On n32 `long` is 32 bits, so formatting one
with `%ld` silently drops the high word.

## Runner identity

The runner reports an OS in three places. Only one matters.

`runnerOS` in the [`acquirejob`](#post-run_service_urlacquirejob) body and `os=`
in the message poll query are **not validated**. Sending `IRIX` for both was
tested against the live service: the job was dispatched normally, ran, and
completed successfully. Nothing rejected it and nothing behaved differently.

What matters is the **`Linux` system label** recorded at registration. That is
what `runs-on: [self-hosted, linux, x64]` matches against, server side, so
dropping it stops ordinary workflow YAML from scheduling here.

`RUNNER_OS` and the `runner` context are ours to choose, since
`contextData.runner` arrives null and the runner synthesises it. Keeping the
`Linux` label for schedulability while telling steps the truth is what
`SGUG_RUNNER_OS=IRIX` does. It is off by default because a failing
`runner.os == 'Linux'` test in a third-party action usually falls through to
macOS or Windows handling, which fits IRIX worse than the Linux path.

## Secrets

`variables.github_token` and `variables["system.github.token"]` contain a live
`ghs_` installation token. The encoded form is about 525 bytes, so a 512 byte
buffer truncates it.

The `mask` array is the service telling the runner what to redact from logs. It
contains both regexes and literal secret fragments.

Never persist a captured job message.

## Appendix: no JavaScript actions

Decided against, July 2026, recorded so it is not relitigated.

Node is not reachable. V8 dropped its MIPS backends in 2023, and Node sits on
libuv, which has no IRIX backend and does not list IRIX even as a best-effort
target. The intuition that small devices run Node does not transfer: those are
Linux on little-endian MIPS or ARM. The obstacles here are the OS, the byte
order, and the absence of futex, `MAP_ANON` and working `__thread`, not CPU
speed.

QuickJS would run. Plain C99, no JIT and so no writable-executable pages, no
futex, no libuv. Two to three weeks including the big-endian shakeout, and a
working `qjs` would be useful to SGUG on its own terms.

The expensive part is the `node:` compatibility layer, roughly three months of
`fs`, `child_process`, `stream` and `Buffer`. Even then `actions/checkout`
needs forking, because `@actions/github` pulls `undici` in at module load and
wants `http`, `https`, `tls`, `zlib` and web streams.

The actions an IRIX CI needs, `checkout` and the artifact pair, are implemented
natively in a few hundred lines each. `setup-node`, `setup-python` and
`setup-go` are meaningless here. What JavaScript buys is the long tail, and
that tail assumes Linux binaries.

Porting QuickJS to IRIX remains worthwhile. It is a separate project.
