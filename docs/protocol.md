# Protocol notes

What github.com actually does, as measured against a live org and repository in
July 2026. Everything here contradicts or refines the public reference material
and the two open reimplementations, so it is recorded rather than rediscovered.

Fixture: `test/fixtures/job-message.json`, modelled field for field on a real
captured message with synthetic values throughout.

## Registration

`POST https://api.github.com/actions/runner-registration`, authorised
`Authorization: RemoteAuth <registration token>`, body
`{"url":"https://github.com/<org>","runner_event":"register"}`.

The response for this org contained **no `use_v2_flow` field at all**, and the
agent-add response carried no `ServerUrlV2`. Both suggest the v1 pool flow, and
both are misleading: see Broker migration below.

`authorizationUrl` came back as
`https://tokenghub.actions.githubusercontent.com/_apis/oauth2/token/<guid>`,
not the `vstoken.actions.githubusercontent.com` host the reference names. Use
the returned value verbatim; do not reconstruct it.

`requireFipsCryptography` was `"False"` (the string, inside a
`Dictionary<string,string>`), so assertions are RS256 rather than PS256. Both
must be implemented since the value is per-deployment.

## OAuth

`grant_type=client_credentials` with
`client_assertion_type=urn:ietf:params:oauth:client-assertion-type:jwt-bearer`.
The jwt-bearer URN is the assertion type, never the grant type.

**The assertion lifetime is measured as `exp - nbf` and capped at exactly 300
seconds.** Backdating `nbf` by 30s for clock skew while computing `exp` from
`now` produces a 330 second window, which is rejected as:

```
{"error":"invalid_client","error_description":
 "Bearer token lifetime 00:05:30 is greater than the allowed maximum lifetime 00:05:00"}
```

That description is only visible if the error body is printed; the runner
otherwise sees a bare 400 and looks like it has bad credentials. Derive `exp`
from `nbf`, not from `now`.

The header carries `alg` and `typ` only. No `kid`, no `x5t`, no `iat`.

## Session

`POST {tenant}_apis/distributedtask/pools/{poolId}/sessions` with
`useFipsEncryption: false`.

**The session key is wrapped with RSA-OAEP-SHA256, not SHA-1.** The reference
says the digest follows `useFipsEncryption`, so SHA-1 when false. Measured
against a live session, SHA-1 fails and SHA-256 returns the expected 32 bytes.
SHA-1 is retained as a fallback for older GHES.

A 409 means a session for this agent already exists. That is usually
self-inflicted: see Session lifecycle.

## Broker migration

The service sends a `BrokerMigration` message carrying
`{"brokerBaseUrl":"https://broker.actions.githubusercontent.com"}` **even when
registration reported no v2 flow**. A runner that treats it as an ordinary
message keeps polling the pool, receives nothing but repeated migration
notices, and never receives a job. It looks exactly like an idle runner.

Once migrated:

| | Pool | Broker |
|---|---|---|
| Session create | `POST .../pools/{id}/sessions`, 200 | `POST {broker}/session`, **201** |
| Session delete | `DELETE .../sessions/{sessionId}` | `DELETE {broker}/session?sessionId=`, 204 |
| Poll | `GET .../pools/{id}/messages` | `GET {broker}/message` |
| `messageId` | monotonic int64 | **random 32-bit, often negative** |
| Message delete | `DELETE .../messages/{id}` | none; `POST {broker}/acknowledge` for job refs only |
| `encryptionKey` | present, messages AES-CBC | **absent, messages in clear** |
| `api-version` | required in `Accept` | not used |

The broker also does not accept the api-version media type parameter; plain
`Accept: application/json`.

## Session lifecycle

Two mistakes each cost about a minute of 409 retries on every restart:

1. Migrating to the broker without deleting the pool session first. The orphan
   is addressed by the old endpoint and outlives the process.
2. Letting the shutdown abort check apply to the session delete itself. The
   check is what makes a blocked long poll return promptly on SIGTERM, but it
   cancels the cleanup request too, so the session always leaks. Clear the
   abort check before cleanup: it has to outlive the shutdown it is part of.

## Job delivery

The broker delivers a reference, not the job:

```json
{"runner_request_id":"020d3759-...","run_service_url":"https://run-actions-2-azure-eastus.actions.githubusercontent.com/116/",
 "billing_owner_id":"O_...","should_acknowledge":true}
```

Acknowledge it, then `POST {run_service_url}acquirejob` with
`{"jobMessageId":<runner_request_id>,"runnerOS":"Linux","billingOwnerId":<id>}`.
404, 409 and 422 all mean the job is gone or was won by another runner and are
not worth retrying.

**Acquiring a job takes a parallelism slot that is only released by reporting
completion.** A runner that acquires and then exits leaves the agent with
`currentParallelism: 1` and `maxParallelism: 1`, after which nothing is ever
dispatched to it again. The agent still reports `status: online`. This does not
appear to time out on its own.

`JobCancellation` for such an abandoned job is redelivered on every poll and is
answered immediately rather than long-polled, so a naive loop issues roughly
fifteen requests a second forever. Measured 415 messages in 30 seconds.

## The job message

Two different serialisations appear in the same message.

**Steps use the verbose TemplateToken form:**

```json
{"type": 2, "map": [
  {"Key":   {"type": 0, "lit": "script"},
   "Value": {"type": 0, "file": 1, "line": 21, "col": 14, "lit": "uname -a\n"}}]}
```

**contextData uses the compact PipelineContextData form, and its values are raw
JSON rather than tagged objects:**

```json
{"t": 2, "d": [{"k": "ref", "v": "refs/heads/main"},
               {"k": "check_run_id", "v": 90005817181}]}
```

So a parser needs both `type`/`lit`/`map`/`Key`/`Value` and `t`/`s`/`d`/`k`/`v`.
The reference documentation describes only the compact form.

Other differences from the reference:

- `plan.planType` is `"actions"`, not `"Build"`. It is the hub name in the
  timeline and log URLs.
- `timeline.id` is the same GUID as `plan.planId`.
- `requestId` is `0` and `lockedUntil` is `0001-01-01T00:00:00`, .NET's
  `DateTime.MinValue`. The v1 job-request lock renewal does not apply; use
  `POST {run_service_url}renewjob`.
- `contextData.runner` is `null`. The runner context has to be synthesised
  locally.
- `messageType` on the acquired message stays `RunnerJobRequest`.

### Reporting: the timeline API is gone

This is the single biggest divergence from the reference material, and it is
invisible until nothing appears in the UI.

**Modern github.com does not serve the Azure DevOps timeline API for these
plans.** Extracting the UTF-16 string heap from the official runner v2.336.0
assemblies finds **no `_apis/distributedtask` routes at all**. Every
`PATCH .../timelines/{id}/records`, `POST .../logs` and `POST .../events` call
returns 404, silently, while steps run perfectly well locally.

What the runner actually uses:

| Surface | Base | Verbs |
|---|---|---|
| Run service | `resources.endpoints[0].url` | `session`, `message`, `acquirejob`, `renewjob`, `completejob`, `acknowledge` |
| Results service | `data.ResultsServiceUrl` | Twirp, see below |

Job and per-step state is carried in the `completejob` payload, not by the
timeline:

```
POST {run_service_url}completejob
{"planId":..., "jobId":..., "conclusion":"succeeded",
 "stepResults":[{"external_id":..., "number":1, "name":..., "status":"completed",
                 "conclusion":"succeeded", "started_at":..., "completed_at":...,
                 "completed_log_url":null, "completed_log_lines":42,
                 "annotations":[]}],
 "annotations":[], "environmentUrl":null, "billingOwnerId":...}
```

Sending this is what makes the job go green and what releases the parallelism
slot. Without it the job runs, succeeds locally, and stays "in progress"
forever.

The run service takes no api-version.

### Results service, for logs

Twirp over JSON. Two services, from the runner's string heap:

```
twirp/results.services.receiver.Receiver/
twirp/github.actions.results.api.v1.WorkflowStepUpdateService/
```

Log upload is a three-step dance per step:

1. `GetStepLogsSignedBlobURL` returns a signed Azure blob URL
2. `PUT` the log bytes to that URL
3. `CreateStepLogsMetadata` registers the upload against the step

The same pair exists for whole-job logs, `GetJobLogsSignedBlobURL` and
`CreateJobLogsMetadata`, and for step summaries.

Until this is implemented, steps appear in the UI with the right names and
results but cannot be expanded, because there is no log to show.

### Reporting endpoints

`resources.endpoints[0]` is `SystemVssConnection`. Its
`authorization.parameters.AccessToken` is the token for all job reporting, not
the runner's own OAuth token. Its `data` carries the service addresses:

```
PipelinesServiceUrl   base for _apis/distributedtask/hubs/{planType}/plans/{planId}/...
ResultsServiceUrl     results receiver
CacheServerUrl        actions cache
FeedStreamUrl         wss:// live console, optional; the REST feed works
```

### Secrets

`variables.github_token` and `variables["system.github.token"]` contain a live
`ghs_` installation token. The `mask` array is the service telling the runner
what to redact from logs, and contains both regexes and literal secret
fragments. Never persist a captured job message.
