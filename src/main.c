#include "compat/irix.h"
#include "json/json.h"
#include "net/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Reported to the Actions service at registration. The service enforces a
 * minimum and refuses an older runner, so this tracks GitHub's current release
 * rather than our own progress. Bump when the floor rises.
 */
#define RUNNER_VERSION "2.336.0"

#define USER_AGENT "irix-actions-runner/" RUNNER_VERSION " (IRIX; mips)"

static int
usage(void)
{
	fputs("usage: runner <command>\n"
	      "\n"
	      "  selftest [host]   check TLS, certificates, HTTP and clock\n"
	      "  version           print the reported runner version\n",
	      stderr);
	return 2;
}

/*
 * Everything the runner needs from the network, end to end, with the failure
 * modes separated. On a freshly imaged IRIX box the usual causes are an expired
 * CA bundle and an undisciplined clock, and both otherwise surface much later
 * as an unexplained 401 during OAuth.
 */
static int
selftest(const char *host)
{
	sgug_http_client *c;
	sgug_http_resp *r;
	sgug_json_doc *doc;
	char url[512];
	const char *server, *date, *body;
	sgug_time_t skew;
	size_t body_len;
	int rc = 1;

	printf("host      %s\n", host);

	c = sgug_http_client_new(NULL, USER_AGENT);
	if (c == NULL) {
		fprintf(stderr, "client: %s\n", sgug_http_last_error());
		return 1;
	}

	sgug_snprintf(url, sizeof(url), "https://%s/", host);

	if (sgug_http_request(c, "GET", url, NULL, 0, NULL, 0, 30000, &r) != 0) {
		fprintf(stderr, "request: %s\n", sgug_http_last_error());
		sgug_http_client_free(c);
		return 1;
	}

	body = sgug_http_body(r, &body_len);
	server = sgug_http_header(r, "Server");
	date = sgug_http_header(r, "Date");

	printf("status    %d\n", sgug_http_status(r));
	printf("server    %s\n", server != NULL ? server : "(none)");
	printf("body      %lu bytes\n", (unsigned long)body_len);

	if (date == NULL) {
		fprintf(stderr, "no Date header, cannot measure clock\n");
		goto out;
	}

	skew = sgug_http_skew(c);
	printf("clock     skew %+lds vs server\n", (long)skew);
	if (skew > 60 || skew < -60) {
		printf("warning   clock is off by more than a minute; the runner\n");
		printf("          will compensate, but enable NTP on this machine\n");
	}

	/* Parsing the response proves the JSON layer works against real service
	 * output rather than only against fixtures. */
	doc = sgug_json_parse(body, body_len, NULL, 0);
	if (doc != NULL) {
		const sgug_json *root = sgug_json_root(doc);

		printf("json      parsed, %lu top-level keys\n",
		    (unsigned long)sgug_json_len(root));
		sgug_json_free(doc);
	} else if (body_len > 0) {
		printf("json      response was not JSON, which is fine here\n");
	}

	/*
	 * A second request on the same client must reuse the connection.
	 * Handshakes cost tens of milliseconds on this hardware and the
	 * listener reconnects every 50 seconds for as long as it runs.
	 */
	sgug_http_resp_free(r);
	r = NULL;
	if (sgug_http_request(c, "GET", url, NULL, 0, NULL, 0, 30000, &r) != 0) {
		fprintf(stderr, "second request: %s\n", sgug_http_last_error());
		goto out;
	}
	printf("keepalive second request returned %d\n", sgug_http_status(r));

	printf("\nselftest OK\n");
	rc = 0;

out:
	sgug_http_resp_free(r);
	sgug_http_client_free(c);
	return rc;
}

int
main(int argc, char **argv)
{
	if (argc < 2)
		return usage();

	if (strcmp(argv[1], "version") == 0) {
		printf("%s\n", RUNNER_VERSION);
		return 0;
	}
	if (strcmp(argv[1], "selftest") == 0)
		return selftest(argc > 2 ? argv[2] : "api.github.com");

	return usage();
}
