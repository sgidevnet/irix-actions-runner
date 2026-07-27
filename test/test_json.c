#include "json/json.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++; \
		} \
	} while (0)

#define CHECK_EQ_STR(got, want) \
	do { \
		const char *g = (got); \
		if (g == NULL || strcmp(g, (want)) != 0) { \
			printf("FAIL %s:%d: %s = \"%s\", want \"%s\"\n", \
			    __FILE__, __LINE__, #got, g ? g : "(null)", (want)); \
			failures++; \
		} \
	} while (0)

#define CHECK_EQ_I64(got, want) \
	do { \
		long long g = (long long)(got); \
		long long wv = (long long)(want); \
		if (g != wv) { \
			printf("FAIL %s:%d: %s = %lld, want %lld\n", \
			    __FILE__, __LINE__, #got, g, wv); \
			failures++; \
		} \
	} while (0)

static sgug_json_doc *
parse(const char *s)
{
	char err[128];
	sgug_json_doc *d = sgug_json_parse(s, strlen(s), err, sizeof(err));

	if (d == NULL)
		printf("  parse error: %s\n", err);
	return d;
}

static void
test_scalars(void)
{
	sgug_json_doc *d = parse(
	    "{\"s\":\"hi\",\"n\":42,\"neg\":-7,\"f\":1.5,\"t\":true,"
	    "\"fa\":false,\"nul\":null}");
	const sgug_json *r;

	CHECK(d != NULL);
	if (d == NULL)
		return;
	r = sgug_json_root(d);

	CHECK_EQ_STR(sgug_json_string(sgug_json_get(r, "s"), NULL), "hi");
	CHECK_EQ_I64(sgug_json_int(sgug_json_get(r, "n"), -1), 42);
	CHECK_EQ_I64(sgug_json_int(sgug_json_get(r, "neg"), 0), -7);
	CHECK(sgug_json_double(sgug_json_get(r, "f"), 0) == 1.5);
	CHECK(sgug_json_bool(sgug_json_get(r, "t"), 0) == 1);
	CHECK(sgug_json_bool(sgug_json_get(r, "fa"), 1) == 0);
	CHECK(sgug_json_type_of(sgug_json_get(r, "nul")) == SGUG_JSON_NULL);

	/* Missing keys fall back rather than crash, so optional protocol fields
	 * need no separate presence check. */
	CHECK_EQ_STR(sgug_json_string(sgug_json_get(r, "absent"), "dflt"), "dflt");
	CHECK_EQ_I64(sgug_json_int(sgug_json_get(r, "absent"), 99), 99);

	sgug_json_free(d);
}

static void
test_case_insensitive_keys(void)
{
	/* The wire is camelCase, the C# contracts are PascalCase, and .runner
	 * files use PascalCase. Both reference implementations match
	 * case-insensitively, so we must too. */
	sgug_json_doc *d = parse("{\"messageType\":\"RunnerJobRequest\",\"AgentId\":42}");
	const sgug_json *r;

	CHECK(d != NULL);
	if (d == NULL)
		return;
	r = sgug_json_root(d);

	CHECK_EQ_STR(sgug_json_string(sgug_json_get(r, "messageType"), NULL),
	    "RunnerJobRequest");
	CHECK_EQ_STR(sgug_json_string(sgug_json_get(r, "MessageType"), NULL),
	    "RunnerJobRequest");
	CHECK_EQ_STR(sgug_json_string(sgug_json_get(r, "MESSAGETYPE"), NULL),
	    "RunnerJobRequest");
	CHECK_EQ_I64(sgug_json_int(sgug_json_get(r, "agentid"), -1), 42);

	sgug_json_free(d);
}

static void
test_bool_from_string(void)
{
	/* .credentials Data and PropertiesCollection are
	 * Dictionary<string,string> on the C# side, so booleans arrive quoted.
	 * Reading requireFipsCryptography as absent would silently pick RS256
	 * where the service expects PS256. */
	sgug_json_doc *d = parse(
	    "{\"requireFipsCryptography\":\"True\",\"enable\":\"true\","
	    "\"off\":\"False\",\"real\":true}");
	const sgug_json *r;

	CHECK(d != NULL);
	if (d == NULL)
		return;
	r = sgug_json_root(d);

	CHECK(sgug_json_bool(sgug_json_get(r, "requireFipsCryptography"), 0) == 1);
	CHECK(sgug_json_bool(sgug_json_get(r, "enable"), 0) == 1);
	CHECK(sgug_json_bool(sgug_json_get(r, "off"), 1) == 0);
	CHECK(sgug_json_bool(sgug_json_get(r, "real"), 0) == 1);

	sgug_json_free(d);
}

static void
test_int64_range(void)
{
	/* messageId is a genuine 64-bit value; narrowing it would break message
	 * acknowledgement. */
	sgug_json_doc *d = parse(
	    "{\"messageId\":1234567890123,\"big\":9223372036854775807,"
	    "\"reqId\":987654}");
	const sgug_json *r;

	CHECK(d != NULL);
	if (d == NULL)
		return;
	r = sgug_json_root(d);

	CHECK_EQ_I64(sgug_json_int(sgug_json_get(r, "messageId"), 0),
	    1234567890123LL);
	CHECK_EQ_I64(sgug_json_int(sgug_json_get(r, "big"), 0),
	    9223372036854775807LL);
	CHECK_EQ_I64(sgug_json_int(sgug_json_get(r, "reqId"), 0), 987654);

	sgug_json_free(d);
}

static void
test_arrays_and_paths(void)
{
	sgug_json_doc *d = parse(
	    "{\"authorization\":{\"publicKey\":{\"exponent\":\"AQAB\","
	    "\"modulus\":\"AbCd\"},\"clientId\":\"guid\"},"
	    "\"labels\":[{\"name\":\"self-hosted\"},{\"name\":\"Linux\"},"
	    "{\"name\":\"irix\"}]}");
	const sgug_json *r, *labels;

	CHECK(d != NULL);
	if (d == NULL)
		return;
	r = sgug_json_root(d);

	CHECK_EQ_STR(sgug_json_string(
	    sgug_json_path(r, "authorization.publicKey.exponent"), NULL), "AQAB");
	CHECK_EQ_STR(sgug_json_string(
	    sgug_json_path(r, "AUTHORIZATION.publickey.MODULUS"), NULL), "AbCd");
	CHECK(sgug_json_path(r, "authorization.publicKey.absent") == NULL);
	CHECK(sgug_json_path(r, "authorization.clientId.deeper") == NULL);

	labels = sgug_json_get(r, "labels");
	CHECK_EQ_I64(sgug_json_len(labels), 3);
	CHECK_EQ_STR(sgug_json_string(
	    sgug_json_get(sgug_json_at(labels, 2), "name"), NULL), "irix");
	CHECK(sgug_json_at(labels, 3) == NULL);

	sgug_json_free(d);
}

static void
test_escapes(void)
{
	sgug_json_doc *d = parse(
	    "{\"a\":\"line\\nbreak\",\"b\":\"quote\\\"here\","
	    "\"c\":\"tab\\there\",\"d\":\"\\u0041\\u00e9\\u4e2d\","
	    "\"e\":\"\\ud83d\\ude00\",\"f\":\"back\\\\slash\"}");
	const sgug_json *r;

	CHECK(d != NULL);
	if (d == NULL)
		return;
	r = sgug_json_root(d);

	CHECK_EQ_STR(sgug_json_string(sgug_json_get(r, "a"), NULL), "line\nbreak");
	CHECK_EQ_STR(sgug_json_string(sgug_json_get(r, "b"), NULL), "quote\"here");
	CHECK_EQ_STR(sgug_json_string(sgug_json_get(r, "c"), NULL), "tab\there");
	CHECK_EQ_STR(sgug_json_string(sgug_json_get(r, "f"), NULL), "back\\slash");

	/* UTF-8 byte sequences, spelled out so a byte-order bug in the encoder
	 * shows as a mismatch rather than passing on one host. */
	CHECK_EQ_STR(sgug_json_string(sgug_json_get(r, "d"), NULL),
	    "\x41\xc3\xa9\xe4\xb8\xad");
	/* Surrogate pair for U+1F600. */
	CHECK_EQ_STR(sgug_json_string(sgug_json_get(r, "e"), NULL),
	    "\xf0\x9f\x98\x80");

	sgug_json_free(d);
}

static void
test_rejects_malformed(void)
{
	char err[128];
	static const char *BAD[] = {
		"{", "}", "[", "{\"a\"}", "{\"a\":}", "{a:1}", "{\"a\":1,}",
		"[1,2", "tru", "{\"a\":01x}", "\"unterminated", "{\"a\":\"\\q\"}",
		"{} trailing", ""
	};
	size_t i;

	for (i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
		sgug_json_doc *d = sgug_json_parse(BAD[i], strlen(BAD[i]),
		    err, sizeof(err));

		if (d != NULL) {
			printf("FAIL: accepted malformed input \"%s\"\n", BAD[i]);
			failures++;
			sgug_json_free(d);
		} else {
			CHECK(err[0] != '\0');
		}
	}
}

static void
test_deep_nesting_bounded(void)
{
	char deep[4096];
	char err[128];
	sgug_json_doc *d;
	int i;

	/* Beyond the depth limit this must fail cleanly rather than exhaust the
	 * stack. */
	for (i = 0; i < 2000; i++)
		deep[i] = '[';
	deep[2000] = '\0';

	d = sgug_json_parse(deep, strlen(deep), err, sizeof(err));
	CHECK(d == NULL);
	sgug_json_free(d);
}

static void
test_bom_tolerated(void)
{
	const char *with_bom = "\xef\xbb\xbf{\"a\":1}";
	sgug_json_doc *d = sgug_json_parse(with_bom, strlen(with_bom), NULL, 0);

	CHECK(d != NULL);
	if (d != NULL) {
		CHECK_EQ_I64(sgug_json_int(
		    sgug_json_get(sgug_json_root(d), "a"), 0), 1);
		sgug_json_free(d);
	}
}

static void
test_writer(void)
{
	sgug_jsonw *w = sgug_jsonw_new();
	const char *out;

	CHECK(w != NULL);
	if (w == NULL)
		return;

	/* The shape of the agent registration body. */
	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "name");
	sgug_jsonw_str(w, "octane");
	sgug_jsonw_key(w, "version");
	sgug_jsonw_str(w, "2.336.0");
	sgug_jsonw_key(w, "maxParallelism");
	sgug_jsonw_int(w, 1);
	sgug_jsonw_key(w, "ephemeral");
	sgug_jsonw_bool(w, 0);
	sgug_jsonw_key(w, "disableUpdate");
	sgug_jsonw_bool(w, 1);
	sgug_jsonw_key(w, "authorization");
	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "publicKey");
	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "exponent");
	sgug_jsonw_str(w, "AQAB");
	sgug_jsonw_obj_end(w);
	sgug_jsonw_obj_end(w);
	sgug_jsonw_key(w, "labels");
	sgug_jsonw_arr_begin(w);
	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "name");
	sgug_jsonw_str(w, "self-hosted");
	sgug_jsonw_key(w, "type");
	sgug_jsonw_str(w, "system");
	sgug_jsonw_obj_end(w);
	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "name");
	sgug_jsonw_str(w, "irix");
	sgug_jsonw_key(w, "type");
	sgug_jsonw_str(w, "user");
	sgug_jsonw_obj_end(w);
	sgug_jsonw_arr_end(w);
	sgug_jsonw_obj_end(w);

	out = sgug_jsonw_done(w, NULL);
	CHECK_EQ_STR(out,
	    "{\"name\":\"octane\",\"version\":\"2.336.0\",\"maxParallelism\":1,"
	    "\"ephemeral\":false,\"disableUpdate\":true,"
	    "\"authorization\":{\"publicKey\":{\"exponent\":\"AQAB\"}},"
	    "\"labels\":[{\"name\":\"self-hosted\",\"type\":\"system\"},"
	    "{\"name\":\"irix\",\"type\":\"user\"}]}");

	sgug_jsonw_free(w);
}

static void
test_writer_escapes(void)
{
	sgug_jsonw *w = sgug_jsonw_new();
	const char *out;

	CHECK(w != NULL);
	if (w == NULL)
		return;

	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "script");
	/* A run: step body is arbitrary shell and routinely contains quotes,
	 * backslashes and newlines. */
	sgug_jsonw_str(w, "echo \"hi\"\nmake\\all\ttab");
	sgug_jsonw_key(w, "ctl");
	sgug_jsonw_str(w, "\001");
	sgug_jsonw_obj_end(w);

	out = sgug_jsonw_done(w, NULL);
	CHECK_EQ_STR(out,
	    "{\"script\":\"echo \\\"hi\\\"\\nmake\\\\all\\ttab\","
	    "\"ctl\":\"\\u0001\"}");

	sgug_jsonw_free(w);
}

static void
test_writer_roundtrip(void)
{
	sgug_jsonw *w = sgug_jsonw_new();
	sgug_json_doc *d;
	const char *out;

	CHECK(w != NULL);
	if (w == NULL)
		return;

	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "utf8");
	sgug_jsonw_str(w, "caf\xc3\xa9 \xe4\xb8\xad");
	sgug_jsonw_key(w, "n");
	sgug_jsonw_int(w, 1234567890123LL);
	sgug_jsonw_key(w, "empty");
	sgug_jsonw_obj_begin(w);
	sgug_jsonw_obj_end(w);
	sgug_jsonw_key(w, "arr");
	sgug_jsonw_arr_begin(w);
	sgug_jsonw_arr_end(w);
	sgug_jsonw_obj_end(w);

	out = sgug_jsonw_done(w, NULL);
	CHECK(out != NULL);
	if (out == NULL) {
		sgug_jsonw_free(w);
		return;
	}

	d = parse(out);
	CHECK(d != NULL);
	if (d != NULL) {
		const sgug_json *r = sgug_json_root(d);

		CHECK_EQ_STR(sgug_json_string(sgug_json_get(r, "utf8"), NULL),
		    "caf\xc3\xa9 \xe4\xb8\xad");
		CHECK_EQ_I64(sgug_json_int(sgug_json_get(r, "n"), 0),
		    1234567890123LL);
		CHECK(sgug_json_type_of(sgug_json_get(r, "empty")) ==
		    SGUG_JSON_OBJECT);
		CHECK_EQ_I64(sgug_json_len(sgug_json_get(r, "arr")), 0);
		sgug_json_free(d);
	}

	sgug_jsonw_free(w);
}

/* A trimmed but structurally faithful AgentJobRequestMessage. */
static void
test_job_message_shape(void)
{
	static const char *MSG =
	    "{\"messageType\":\"PipelineAgentJobRequest\","
	    "\"plan\":{\"scopeIdentifier\":\"00000000-0000-0000-0000-000000000000\","
	    "\"planId\":\"aaaa\",\"planType\":\"Build\",\"version\":12},"
	    "\"timeline\":{\"id\":\"bbbb\",\"changeId\":1,\"location\":null},"
	    "\"jobId\":\"cccc\",\"jobDisplayName\":\"build\",\"requestId\":987654,"
	    "\"resources\":{\"endpoints\":[{\"name\":\"SystemVssConnection\","
	    "\"url\":\"https://example/\",\"authorization\":{\"scheme\":\"OAuth\","
	    "\"parameters\":{\"AccessToken\":\"eyJtok\"}},"
	    "\"data\":{\"FeedStreamUrl\":\"https://feed/\"}}]},"
	    "\"steps\":[{\"id\":\"dddd\",\"type\":\"action\","
	    "\"reference\":{\"type\":\"script\"},\"contextName\":\"__run\","
	    "\"condition\":\"success()\","
	    "\"inputs\":{\"t\":2,\"d\":[{\"k\":\"script\",\"v\":{\"t\":0,"
	    "\"s\":\"make -j2\"}}]}}]}";
	sgug_json_doc *d = parse(MSG);
	const sgug_json *r, *ep, *steps, *step, *inputs, *dlist;

	CHECK(d != NULL);
	if (d == NULL)
		return;
	r = sgug_json_root(d);

	CHECK_EQ_STR(sgug_json_string(sgug_json_get(r, "messageType"), NULL),
	    "PipelineAgentJobRequest");
	CHECK_EQ_STR(sgug_json_string(sgug_json_path(r, "plan.planType"), NULL),
	    "Build");
	CHECK_EQ_I64(sgug_json_int(sgug_json_get(r, "requestId"), 0), 987654);
	CHECK(sgug_json_type_of(sgug_json_path(r, "timeline.location")) ==
	    SGUG_JSON_NULL);

	/* The job reporting token lives here, not in the runner credentials. */
	ep = sgug_json_at(sgug_json_path(r, "resources.endpoints"), 0);
	CHECK_EQ_STR(sgug_json_string(sgug_json_get(ep, "name"), NULL),
	    "SystemVssConnection");
	CHECK_EQ_STR(sgug_json_string(
	    sgug_json_path(ep, "authorization.parameters.AccessToken"), NULL),
	    "eyJtok");

	/* A run: step carries its shell body in the PipelineContextData
	 * dictionary under key "script". */
	steps = sgug_json_get(r, "steps");
	CHECK_EQ_I64(sgug_json_len(steps), 1);
	step = sgug_json_at(steps, 0);
	CHECK_EQ_STR(sgug_json_string(
	    sgug_json_path(step, "reference.type"), NULL), "script");

	inputs = sgug_json_get(step, "inputs");
	CHECK_EQ_I64(sgug_json_int(sgug_json_get(inputs, "t"), -1), 2);
	dlist = sgug_json_get(inputs, "d");
	CHECK_EQ_STR(sgug_json_string(
	    sgug_json_get(sgug_json_at(dlist, 0), "k"), NULL), "script");
	CHECK_EQ_STR(sgug_json_string(
	    sgug_json_path(sgug_json_at(dlist, 0), "v.s"), NULL), "make -j2");

	sgug_json_free(d);
}

int
main(void)
{
	test_scalars();
	test_case_insensitive_keys();
	test_bool_from_string();
	test_int64_range();
	test_arrays_and_paths();
	test_escapes();
	test_rejects_malformed();
	test_deep_nesting_bounded();
	test_bom_tolerated();
	test_writer();
	test_writer_escapes();
	test_writer_roundtrip();
	test_job_message_shape();

	if (failures != 0) {
		printf("\n%d failure(s)\n", failures);
		return 1;
	}
	printf("all json tests passed\n");
	return 0;
}
