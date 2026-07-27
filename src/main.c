#include "compat/irix.h"
#include "net/tcp.h"
#include "net/tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Reported to the Actions service at registration. The service enforces a
 * minimum and will refuse an older runner, so this tracks GitHub's current
 * release rather than our own progress. Bump when the floor rises.
 */
#define RUNNER_VERSION "2.336.0"

static int
usage(void)
{
	fputs("usage: runner <command>\n"
	      "\n"
	      "  selftest [host]   check TLS reachability, certificates and clock\n"
	      "  version           print the reported runner version\n",
	      stderr);
	return 2;
}

/*
 * Everything the runner needs from the network, end to end, with the failures
 * separated. On a freshly imaged IRIX box the usual causes are an expired CA
 * bundle and an unset clock, and both produce confusing errors much later
 * during OAuth if not caught here.
 */
static int
selftest(const char *host)
{
	sgug_tls_ctx *ctx;
	sgug_tls *tls;
	sgug_time_t local, remote;
	char req[256];
	char buf[4096];
	int fd, n, len;
	char *date;

	printf("host      %s\n", host);

	ctx = sgug_tls_ctx_new(NULL);
	if (ctx == NULL) {
		fprintf(stderr, "tls context: %s\n", sgug_tls_last_error());
		return 1;
	}

	fd = sgug_tcp_connect(host, 443, 15000);
	if (fd < 0) {
		fprintf(stderr, "connect %s:443 failed\n", host);
		sgug_tls_ctx_free(ctx);
		return 1;
	}
	sgug_tcp_set_nodelay(fd);
	sgug_tcp_set_timeouts(fd, 30000, 30000);

	tls = sgug_tls_connect(ctx, fd, host);
	if (tls == NULL) {
		fprintf(stderr, "handshake: %s\n", sgug_tls_last_error());
		sgug_tls_ctx_free(ctx);
		return 1;
	}

	printf("protocol  %s\n", sgug_tls_version(tls));
	printf("cipher    %s\n", sgug_tls_cipher(tls));
	printf("verify    OK\n");

	len = sgug_snprintf(req, sizeof(req),
	    "HEAD / HTTP/1.1\r\n"
	    "Host: %s\r\n"
	    "User-Agent: irix-actions-runner/%s\r\n"
	    "Connection: close\r\n"
	    "\r\n", host, RUNNER_VERSION);

	if (sgug_tls_write(tls, req, (size_t)len) < 0) {
		fprintf(stderr, "write: %s\n", sgug_tls_last_error());
		goto fail;
	}

	n = sgug_tls_read(tls, buf, sizeof(buf) - 1);
	if (n <= 0) {
		fprintf(stderr, "read: %s\n", sgug_tls_last_error());
		goto fail;
	}
	buf[n] = '\0';

	date = strcasestr(buf, "\r\nDate:");
	if (date == NULL) {
		fprintf(stderr, "no Date header in response\n");
		goto fail;
	}

	local = sgug_now();
	remote = sgug_parse_http_date(date + 7);
	if (remote < 0) {
		fprintf(stderr, "unparseable Date header\n");
		goto fail;
	}

	printf("clock     local %ld, server %ld, skew %+lds\n",
	    (long)local, (long)remote, (long)(local - remote));

	/*
	 * OAuth assertions live five minutes, so anything approaching that
	 * fails every token request with an opaque 401. The runner compensates
	 * using this measurement, but a clock this wrong is worth fixing.
	 */
	if (local - remote > 120 || remote - local > 120)
		printf("warning   clock is off by more than two minutes, enable NTP\n");

	sgug_tls_free(tls);
	sgug_tls_ctx_free(ctx);
	printf("\nselftest OK\n");
	return 0;

fail:
	sgug_tls_free(tls);
	sgug_tls_ctx_free(ctx);
	return 1;
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
