#include "proto/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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
		if (strcmp(g, (want)) != 0) { \
			printf("FAIL %s:%d: %s = \"%s\", want \"%s\"\n", \
			    __FILE__, __LINE__, #got, g, (want)); \
			failures++; \
		} \
	} while (0)

static void
cleanup(const char *dir)
{
	sgug_config_remove(dir);
	rmdir(dir);
}

static void
test_roundtrip(void)
{
	char dir[] = "/tmp/sgugcfgXXXXXX";
	sgug_config in, out;
	char path[512];
	struct stat st;

	if (mkdtemp(dir) == NULL) {
		printf("FAIL: mkdtemp\n");
		failures++;
		return;
	}

	memset(&in, 0, sizeof(in));
	in.agent_id = 68;
	in.pool_id = 3;
	in.ephemeral = 0;
	in.disable_update = 1;
	in.use_v2_flow = 1;
	in.require_fips = 1;
	strcpy(in.agent_name, "octane");
	strcpy(in.pool_name, "IRIX");
	strcpy(in.server_url,
	    "https://pipelinesghubeus21.actions.githubusercontent.com/abc/");
	strcpy(in.server_url_v2, "https://broker.actions.githubusercontent.com/");
	strcpy(in.github_url, "https://github.com/sgidevnet");
	strcpy(in.work_folder, "_work");
	strcpy(in.client_id, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
	strcpy(in.auth_url,
	    "https://tokenghub.actions.githubusercontent.com/_apis/oauth2/token/g");

	CHECK(sgug_config_save(&in, dir) == 0);
	CHECK(sgug_config_exists(dir));

	CHECK(sgug_config_load(&out, dir) == 0);
	CHECK(out.agent_id == 68);
	CHECK(out.pool_id == 3);
	CHECK(out.disable_update == 1);
	CHECK(out.use_v2_flow == 1);
	CHECK_EQ_STR(out.agent_name, "octane");
	CHECK_EQ_STR(out.pool_name, "IRIX");
	CHECK_EQ_STR(out.server_url, in.server_url);
	CHECK_EQ_STR(out.server_url_v2, in.server_url_v2);
	CHECK_EQ_STR(out.github_url, in.github_url);
	CHECK_EQ_STR(out.client_id, in.client_id);
	CHECK_EQ_STR(out.auth_url, in.auth_url);

	/* Written as the string "True" to match the service's own
	 * Dictionary<string,string>, and it has to survive the round trip as a
	 * boolean or the runner silently signs RS256 where PS256 is required. */
	CHECK(out.require_fips == 1);

	/* Credentials must never be group or world readable. */
	sgug_config_path(dir, ".credentials", path, sizeof(path));
	CHECK(stat(path, &st) == 0);
	CHECK((st.st_mode & (S_IRWXG | S_IRWXO)) == 0);

	CHECK(sgug_config_remove(dir) == 0);
	CHECK(!sgug_config_exists(dir));

	cleanup(dir);
}

static void
test_load_missing(void)
{
	sgug_config cfg;

	CHECK(sgug_config_load(&cfg, "/tmp/sgug-does-not-exist-xyz") != 0);
	CHECK(!sgug_config_exists("/tmp/sgug-does-not-exist-xyz"));
}

int
main(void)
{
	test_roundtrip();
	test_load_missing();

	if (failures != 0) {
		printf("\n%d failure(s)\n", failures);
		return 1;
	}
	printf("all config tests passed\n");
	return 0;
}
