/*
 * engine_cli.c -- drive one amd_engine run from a raw 8 kHz slin file in
 * real time, without FreeSWITCH.  Used by test/unit.sh against the mock server.
 *
 *   engine_cli [-h host] [-p port] [-m ws|local|compare] [-F] [-t timeout_ms]
 *              [-c connect_ms] [-s] [-k] [-v] [-after silence|none|hangup:MS]
 *              [-vid VID] file.sln
 *
 * After the file ends: "silence" feeds digital silence, "none" feeds CNG
 * (no media), "hangup:MS" hangs up MS ms after the end.
 * Prints KEY=VALUE lines (STATUS CAUSE ELAPSED ...) and exits 0.
 */
#include "../src/amd_engine.h"
#include "../src/ws_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void logfn(void *ud, amd_log_level_t lvl, const char *msg)
{
	int verbose = *(int *) ud;
	if (verbose || lvl >= AMD_LOG_WARNING) fprintf(stderr, "[%d] %s\n", lvl, msg);
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
	amd_engine_cfg_t c;
	amd_result_t r;
	amd_engine_t *e;
	const char *file = NULL, *after = "none", *vid = "TEST";
	int verbose = 0, i, hang_ms = -1;
	FILE *f;
	int16_t frame[160];
	char json[1024];
	int64_t t0, end_at = 0;

	memset(&c, 0, sizeof(c));
	snprintf(c.host, sizeof(c.host), "127.0.0.1");
	c.port = 2700;
	c.timeout_ms = 10000;
	c.connect_timeout_ms = 10000;
	c.tls_verify = 0;
	{
		int d[] = { 500, 1000, 1500, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000 };
		memcpy(c.schedule, d, sizeof(d));
		c.nschedule = 11;
	}
	c.chunk_bytes = 8000;
	c.fallback_interval_ms = 1000;
	c.eof_no_audio_streak = 2;
	c.eof_wait_ms = 3000;
	local_amd_defaults(&c.local);
	snprintf(c.tag, sizeof(c.tag), "cli");
	c.log = logfn;
	c.log_ud = &verbose;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : "";
		if (!strcmp(a, "-h")) { snprintf(c.host, sizeof(c.host), "%s", v); i++; }
		else if (!strcmp(a, "-p")) { c.port = atoi(v); i++; }
		else if (!strcmp(a, "-t")) { c.timeout_ms = atoi(v); i++; }
		else if (!strcmp(a, "-c")) { c.connect_timeout_ms = atoi(v); i++; }
		else if (!strcmp(a, "-m")) {
			c.mode = !strcmp(v, "local") ? AMD_MODE_LOCAL : !strcmp(v, "compare") ? AMD_MODE_COMPARE : AMD_MODE_WS;
			i++;
		}
		else if (!strcmp(a, "-F")) c.local_fallback = 1;
		else if (!strcmp(a, "-s")) c.tls = 1;
		else if (!strcmp(a, "-k")) c.tls_verify = 1, c.tls_check_hostname = 1;
		else if (!strcmp(a, "-v")) { verbose = 1; c.trace = 1; }
		else if (!strcmp(a, "-after")) { after = v; i++; }
		else if (!strcmp(a, "-vid")) { vid = v; i++; }
		else file = a;
	}
	if (!file || !(f = fopen(file, "rb"))) {
		fprintf(stderr, "usage: engine_cli [options] file.sln\n");
		return 2;
	}
	if (!strncmp(after, "hangup:", 7)) hang_ms = atoi(after + 7);

	json[0] = 0;
	strcat(json, "{\"config\":{\"sample_rate\":8000,\"VID\":");
	amd_json_append_str(json, sizeof(json), vid);
	strcat(json, "}}");

	amd_engine_global_init();
	e = amd_engine_start(&c, json);
	t0 = amdws_now_ms();
	for (i = 0;; i++) {
		size_t n = f ? fread(frame, 2, 160, f) : 0;
		int64_t due = t0 + (int64_t) (i + 1) * 20;
		if (n < 160 && f) {
			fclose(f);
			f = NULL;
			end_at = amdws_now_ms();
		}
		if (n > 0) {
			amd_engine_feed(e, frame, (uint32_t) n, 0);
		} else if (!strcmp(after, "silence")) {
			memset(frame, 0, sizeof(frame));
			amd_engine_feed(e, frame, 160, 0);
		} else {
			memset(frame, 0, sizeof(frame));
			amd_engine_feed(e, frame, 160, 1);
		}
		if (amd_engine_check(e)) break;
		if (hang_ms >= 0 && end_at && amdws_now_ms() - end_at >= hang_ms) {
			amd_engine_hangup(e, NULL);
			break;
		}
		{
			int64_t w = due - amdws_now_ms();
			if (w > 0) sleep_ms((int) w);
		}
	}
	amd_engine_result(e, &r);
	amd_engine_release(e);
	amd_engine_wait_idle(3000);

	printf("STATUS=%s\nCAUSE=%s\nELAPSED=%ld\nSTATS=%ld-%ld-%ld-%ld\nRESPONSE=%s\nSOURCE=%s\n"
		   "CONNECT_MS=%ld\nLOCAL=%s/%s/%ld\nFALLBACK_FROM=%s\nWALL=%lld\n",
		   r.status, r.cause, r.elapsed_ms, r.elapsed_ms, r.audio_ms, r.chunks, r.bytes, r.response, r.source,
		   r.connect_ms, r.local_status, r.local_cause, r.local_ms, r.fallback_from,
		   (long long) (amdws_now_ms() - t0));
	if (f) fclose(f);
	return 0;
}
