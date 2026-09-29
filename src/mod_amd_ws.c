/*
 * mod_amd_ws -- answering machine detection for FreeSWITCH by streaming the
 * first seconds of the call to the amdy.io AMD service over WebSocket.
 *
 * Same protocol, schedule and result vocabulary as app_amd_ws for Asterisk
 * (and the amdy.io EAGI client amd.py, July 2026); the energy detector of
 * mod_amd (Asterisk app_amd, via github.com/seanbright/mod_amd, CC0) is
 * built in for "local", "compare" and fallback operation.
 *
 * Dialplan:   <action application="amd_ws" data="[host[,port[,vid[,timeout_ms[,playfile[,options]]]]]]"/>
 * API:        uuid_amd_ws <uuid> start [same arguments] | uuid_amd_ws <uuid> stop
 *             amd_ws status | amd_ws reload
 * Event:      CUSTOM amd_ws::result
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 1.1.  See LICENSE.
 */
#include <switch.h>

#include "amd_engine.h"
#include "ws_client.h"

#define MOD_AMD_WS_VERSION "1.0.0"
#define AMD_WS_EVENT "amd_ws::result"
#define AMD_WS_PRIVATE "_amd_ws_call_"
#define AMD_WS_SYNTAX "[host[,port[,vid[,timeout_ms[,playfile[,options]]]]]]"
#define UUID_AMD_WS_SYNTAX "<uuid> start [" AMD_WS_SYNTAX "] | <uuid> stop"

SWITCH_MODULE_LOAD_FUNCTION(mod_amd_ws_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_amd_ws_shutdown);
SWITCH_MODULE_DEFINITION(mod_amd_ws, mod_amd_ws_load, mod_amd_ws_shutdown, NULL);

static struct {
	switch_mutex_t *mutex;
	switch_memory_pool_t *pool;
	switch_loadable_module_interface_t *mi;
	amd_engine_cfg_t base;       /* defaults from amd_ws.conf */
	char extra_config[2048];     /* JSON object members spliced into the config frame */
	int send_caller_id;
	int playdelay_ms;
	int answer;                  /* answer an unanswered channel (default yes) */
	char config_file[64];
	int active;                  /* calls in progress */
} globals;

/* One detection on one channel. Allocated from the session pool. */
typedef struct {
	switch_core_session_t *session;
	switch_channel_t *channel;
	switch_media_bug_t *bug;
	amd_engine_t *engine;
	switch_audio_resampler_t *resampler;
	uint32_t in_rate;
	uint32_t in_channels;
	int background;              /* started with uuid_amd_ws: the bug owns the call */
	volatile int playing;
	int applied;
	int stopped;                 /* uuid_amd_ws stop */
	char vid[128];
	char host[256];
	int port;
	amd_mode_t mode;
	char playfile[512];
	int playdelay_ms;
	int64_t t_start;
} amd_call_t;

/* ------------------------------------------------------------------------ */
/* logging bridge                                                             */

static void engine_log(void *ud, amd_log_level_t lvl, const char *msg)
{
	const char *uuid = ud;
	switch_log_level_t l = SWITCH_LOG_DEBUG;
	switch (lvl) {
	case AMD_LOG_DEBUG: l = SWITCH_LOG_DEBUG; break;
	case AMD_LOG_INFO: l = SWITCH_LOG_INFO; break;
	case AMD_LOG_NOTICE: l = SWITCH_LOG_NOTICE; break;
	case AMD_LOG_WARNING: l = SWITCH_LOG_WARNING; break;
	case AMD_LOG_ERROR: l = SWITCH_LOG_ERROR; break;
	}
	switch_log_printf(SWITCH_CHANNEL_UUID_LOG(uuid && *uuid ? uuid : NULL), l, "%s\n", msg);
}

/* ------------------------------------------------------------------------ */
/* configuration                                                              */

static int parse_schedule(const char *v, int *out, int *n)
{
	int cnt = 0, last = 0;
	const char *p = v;
	while (p && *p) {
		char *end;
		long x;
		while (*p == ' ' || *p == ',') p++;
		if (!*p) break;
		x = strtol(p, &end, 10);
		if (end == p || x <= last || x > 600000 || cnt >= AMD_MAX_SCHEDULE) return -1;
		out[cnt++] = (int) x;
		last = (int) x;
		p = end;
	}
	if (!cnt) return -1;
	*n = cnt;
	return 0;
}

static int int_in(const char *v, int lo, int hi, int dflt, const char *name)
{
	char *end;
	long x;
	if (zstr(v)) return dflt;
	x = strtol(v, &end, 10);
	if (*end || x < lo || x > hi) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "amd_ws.conf: %s=%s is not in %d..%d, using %d\n", name, v, lo, hi, dflt);
		return dflt;
	}
	return (int) x;
}

static amd_mode_t parse_mode(const char *v, amd_mode_t dflt)
{
	if (zstr(v)) return dflt;
	if (!strcasecmp(v, "ws") || !strcasecmp(v, "stream")) return AMD_MODE_WS;
	if (!strcasecmp(v, "local")) return AMD_MODE_LOCAL;
	if (!strcasecmp(v, "compare")) return AMD_MODE_COMPARE;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "amd_ws: mode '%s' unknown (ws|local|compare), using default\n", v);
	return dflt;
}

static const char *mode_str(amd_mode_t m)
{
	return m == AMD_MODE_LOCAL ? "local" : m == AMD_MODE_COMPARE ? "compare" : "ws";
}

static void set_defaults(amd_engine_cfg_t *c)
{
	static const int sched[] = { 500, 1000, 1500, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000 };
	memset(c, 0, sizeof(*c));
	switch_copy_string(c->host, "127.0.0.1", sizeof(c->host));
	c->port = 2700;
	switch_copy_string(c->path, "/", sizeof(c->path));
	c->tls_verify = 1;
	c->tls_check_hostname = 1;
	c->timeout_ms = 10000;
	c->connect_timeout_ms = 10000;
	c->result_grace_ms = 0;
	memcpy(c->schedule, sched, sizeof(sched));
	c->nschedule = (int) (sizeof(sched) / sizeof(sched[0]));
	c->chunk_bytes = 8000;
	c->fallback_interval_ms = 1000;
	c->eof_no_audio_streak = 2;
	c->eof_wait_ms = 3000;
	c->mode = AMD_MODE_WS;
	c->local_fallback = 0;
	local_amd_defaults(&c->local);
	c->log = engine_log;
}

static switch_status_t load_config(void)
{
	switch_xml_t xml, cfg, settings, param;
	amd_engine_cfg_t c;
	char extra[sizeof(globals.extra_config)] = "";
	int send_cid = 1, playdelay = 0, answer = 1;

	set_defaults(&c);

	if (!(xml = switch_xml_open_cfg(globals.config_file, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
						  "amd_ws: %s not found, using built-in defaults (host 127.0.0.1:2700)\n", globals.config_file);
	} else {
		if ((settings = switch_xml_child(cfg, "settings"))) {
			for (param = switch_xml_child(settings, "param"); param; param = param->next) {
				const char *n = switch_xml_attr_soft(param, "name");
				const char *v = switch_xml_attr_soft(param, "value");

				if (!strcasecmp(n, "host")) switch_copy_string(c.host, v, sizeof(c.host));
				else if (!strcasecmp(n, "port")) c.port = int_in(v, 1, 65535, 2700, n);
				else if (!strcasecmp(n, "path")) switch_copy_string(c.path, zstr(v) ? "/" : v, sizeof(c.path));
				else if (!strcasecmp(n, "tls")) c.tls = switch_true(v);
				else if (!strcasecmp(n, "tls_verify")) c.tls_verify = switch_true(v);
				else if (!strcasecmp(n, "tls_check_hostname")) c.tls_check_hostname = switch_true(v);
				else if (!strcasecmp(n, "tls_cafile")) switch_copy_string(c.tls_cafile, v, sizeof(c.tls_cafile));
				else if (!strcasecmp(n, "timeout_ms")) c.timeout_ms = int_in(v, 500, 120000, 10000, n);
				else if (!strcasecmp(n, "connect_timeout_ms")) c.connect_timeout_ms = int_in(v, 100, 60000, 10000, n);
				else if (!strcasecmp(n, "result_grace_ms")) c.result_grace_ms = int_in(v, 0, 10000, 0, n);
				else if (!strcasecmp(n, "send_schedule")) {
					if (parse_schedule(v, c.schedule, &c.nschedule) < 0) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
										  "amd_ws.conf: send_schedule=%s invalid (strictly increasing ms, max %d), using the default\n",
										  v, AMD_MAX_SCHEDULE);
						set_defaults(&c); /* restore schedule only */
					}
				}
				else if (!strcasecmp(n, "chunk_bytes")) c.chunk_bytes = int_in(v, 320, 160000, 8000, n);
				else if (!strcasecmp(n, "fallback_interval_ms")) c.fallback_interval_ms = int_in(v, 20, 60000, 1000, n);
				else if (!strcasecmp(n, "eof_no_audio_streak")) c.eof_no_audio_streak = int_in(v, 0, 16, 2, n);
				else if (!strcasecmp(n, "eof_wait_ms")) c.eof_wait_ms = int_in(v, 100, 30000, 3000, n);
				else if (!strcasecmp(n, "send_caller_id")) send_cid = switch_true(v);
				else if (!strcasecmp(n, "extra_config")) {
					const char *s = v;
					size_t l;
					while (*s == ' ') s++;
					l = strlen(s);
					while (l && s[l - 1] == ' ') l--;
					if (l >= 2 && s[0] == '{' && s[l - 1] == '}') {
						cJSON *j = cJSON_Parse(s);
						if (j && cJSON_IsObject(j)) {
							/* members only, without the braces */
							size_t inner = l - 2;
							if (inner >= sizeof(extra)) inner = sizeof(extra) - 1;
							memcpy(extra, s + 1, inner);
							extra[inner] = 0;
						} else {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "amd_ws.conf: extra_config is not a JSON object, ignored\n");
						}
						if (j) cJSON_Delete(j);
					} else if (l) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "amd_ws.conf: extra_config must be a JSON object, ignored\n");
					}
				}
				else if (!strcasecmp(n, "playdelay_ms")) playdelay = int_in(v, 0, 60000, 0, n);
				else if (!strcasecmp(n, "answer")) answer = switch_true(v);
				else if (!strcasecmp(n, "trace")) c.trace = switch_true(v);
				else if (!strcasecmp(n, "mode")) c.mode = parse_mode(v, AMD_MODE_WS);
				else if (!strcasecmp(n, "local_fallback")) c.local_fallback = switch_true(v);
				else if (!strcasecmp(n, "compare_wait_local")) c.compare_wait_local = switch_true(v);
				/* local detector: mod_amd parameter names */
				else if (!strcasecmp(n, "initial_silence")) c.local.initial_silence = (uint32_t) int_in(v, 0, 60000, 2500, n);
				else if (!strcasecmp(n, "greeting")) c.local.greeting = (uint32_t) int_in(v, 0, 60000, 1500, n);
				else if (!strcasecmp(n, "after_greeting_silence")) c.local.after_greeting_silence = (uint32_t) int_in(v, 0, 60000, 800, n);
				else if (!strcasecmp(n, "total_analysis_time")) c.local.total_analysis_time = (uint32_t) int_in(v, 0, 120000, 5000, n);
				else if (!strcasecmp(n, "min_word_length")) c.local.min_word_length = (uint32_t) int_in(v, 0, 60000, 100, n);
				else if (!strcasecmp(n, "between_words_silence")) c.local.between_words_silence = (uint32_t) int_in(v, 0, 60000, 50, n);
				else if (!strcasecmp(n, "maximum_number_of_words")) c.local.maximum_number_of_words = (uint32_t) int_in(v, 1, 100, 3, n);
				else if (!strcasecmp(n, "maximum_word_length")) c.local.maximum_word_length = (uint32_t) int_in(v, 0, 60000, 5000, n);
				else if (!strcasecmp(n, "silence_threshold")) c.local.silence_threshold = (uint32_t) int_in(v, 0, 32767, 256, n);
				else switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "amd_ws.conf: unknown parameter '%s' ignored\n", n);
			}
		}
		switch_xml_free(xml);
	}

	switch_mutex_lock(globals.mutex);
	globals.base = c;
	switch_copy_string(globals.extra_config, extra, sizeof(globals.extra_config));
	globals.send_caller_id = send_cid;
	globals.playdelay_ms = playdelay;
	globals.answer = answer;
	switch_mutex_unlock(globals.mutex);
	return SWITCH_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* arguments                                                                  */

typedef struct {
	const char *host, *port, *vid, *timeout, *playfile, *options;
	/* options */
	int tls, noanswer, answer, trace, fallback, nofallback;
	int have_mode;
	amd_mode_t mode;
	char playdelay[16], connto[16], phone[64], code[16], cid[128];
} amd_args_t;

/* Options: s d(ms) c(ms) p(phone) k(code) i(cid) a A v n  +  L C F f
 * (Asterisk AMD_WS() letters, plus L=local C=compare F=local fallback on f=off). */
static void parse_options(const char *o, amd_args_t *a)
{
	while (o && *o) {
		char letter = *o++;
		char arg[128] = "";
		if (*o == '(') {
			const char *end = strchr(o, ')');
			size_t l;
			if (!end) end = o + strlen(o);
			l = (size_t) (end - o - 1);
			if (l >= sizeof(arg)) l = sizeof(arg) - 1;
			memcpy(arg, o + 1, l);
			arg[l] = 0;
			o = *end ? end + 1 : end;
		}
		switch (letter) {
		case 's': a->tls = 1; break;
		case 'd': switch_copy_string(a->playdelay, arg, sizeof(a->playdelay)); break;
		case 'c': switch_copy_string(a->connto, arg, sizeof(a->connto)); break;
		case 'p': switch_copy_string(a->phone, arg, sizeof(a->phone)); break;
		case 'k': switch_copy_string(a->code, arg, sizeof(a->code)); break;
		case 'i': switch_copy_string(a->cid, arg, sizeof(a->cid)); break;
		case 'a': a->answer = 1; break;
		case 'A': a->noanswer = 1; break;
		case 'v': a->trace = 1; break;
		case 'n': break;                          /* Asterisk: no DB lookup; nothing to skip here */
		case 'L': a->have_mode = 1; a->mode = AMD_MODE_LOCAL; break;
		case 'C': a->have_mode = 1; a->mode = AMD_MODE_COMPARE; break;
		case 'F': a->fallback = 1; break;
		case 'f': a->nofallback = 1; break;
		case ' ': case ',': break;
		default:
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "amd_ws: unknown option '%c' ignored\n", letter);
		}
	}
}

static const char *first_nonempty(const char *a, const char *b)
{
	return !zstr(a) ? a : b;
}

static int valid_cid_value(const char *v)
{
	return !zstr(v) && strcasecmp(v, "Unknown") && strcasecmp(v, "0000000000");
}

/* Build the engine config and the config TEXT frame for this call. */
static void build_call(amd_call_t *call, char *data, amd_engine_cfg_t *c, char *json, size_t jsonlen, int *noanswer)
{
	switch_channel_t *channel = call->channel;
	char *argv[6] = { 0 };
	amd_args_t a;
	const char *v, *phone, *code, *cid;
	char extra[sizeof(globals.extra_config)];
	int send_cid, playdelay, answer;

	memset(&a, 0, sizeof(a));
	if (!zstr(data)) {
		switch_separate_string(data, ',', argv, 6);
	}
	a.host = argv[0]; a.port = argv[1]; a.vid = argv[2]; a.timeout = argv[3]; a.playfile = argv[4]; a.options = argv[5];
	parse_options(a.options, &a);

	switch_mutex_lock(globals.mutex);
	*c = globals.base;
	switch_copy_string(extra, globals.extra_config, sizeof(extra));
	send_cid = globals.send_caller_id;
	playdelay = globals.playdelay_ms;
	answer = globals.answer;
	switch_mutex_unlock(globals.mutex);

	/* precedence: dialplan argument > channel variable > amd_ws.conf */
	if (!zstr(v = first_nonempty(a.host, switch_channel_get_variable(channel, "amd_ws_host")))) switch_copy_string(c->host, v, sizeof(c->host));
	if (!zstr(v = first_nonempty(a.port, switch_channel_get_variable(channel, "amd_ws_port")))) {
		int p = atoi(v);
		if (p > 0 && p < 65536) c->port = p;
		else switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(call->session), SWITCH_LOG_WARNING, "amd_ws: port '%s' invalid, using %d\n", v, c->port);
	}
	if (!zstr(v = first_nonempty(a.timeout, switch_channel_get_variable(channel, "amd_ws_timeout_ms")))) {
		int t = atoi(v);
		if (t > 0) c->timeout_ms = t;
	}
	if (!zstr(v = first_nonempty(a.connto, switch_channel_get_variable(channel, "amd_ws_connect_timeout_ms")))) {
		int t = atoi(v);
		if (t > 0) c->connect_timeout_ms = t;
	}
	if (a.tls || switch_true(switch_channel_get_variable(channel, "amd_ws_tls"))) c->tls = 1;
	if (a.trace || switch_true(switch_channel_get_variable(channel, "amd_ws_trace"))) c->trace = 1;
	if (a.have_mode) c->mode = a.mode;
	else if (!zstr(v = switch_channel_get_variable(channel, "amd_ws_mode"))) c->mode = parse_mode(v, c->mode);
	if (!zstr(v = switch_channel_get_variable(channel, "amd_ws_compare_wait_local"))) c->compare_wait_local = switch_true(v);
	if (a.fallback) c->local_fallback = 1;
	else if (a.nofallback) c->local_fallback = 0;
	else if (!zstr(v = switch_channel_get_variable(channel, "amd_ws_local_fallback"))) c->local_fallback = switch_true(v);
	if (!zstr(v = first_nonempty(a.playdelay, switch_channel_get_variable(channel, "amd_ws_playdelay_ms")))) playdelay = atoi(v);
	if (playdelay < 0) playdelay = 0;
	if (a.noanswer) answer = 0;
	if (a.answer) answer = 1;
	*noanswer = !answer;

	/* VID: argument > amd_ws_vid > caller id name > Unknown */
	v = first_nonempty(a.vid, switch_channel_get_variable(channel, "amd_ws_vid"));
	if (zstr(v)) v = switch_channel_get_variable(channel, "caller_id_name");
	if (!valid_cid_value(v)) v = "Unknown";
	switch_copy_string(call->vid, v, sizeof(call->vid));

	phone = first_nonempty(a.phone, switch_channel_get_variable(channel, "amd_ws_phone"));
	code = first_nonempty(a.code, switch_channel_get_variable(channel, "amd_ws_country_code"));
	cid = first_nonempty(a.cid, switch_channel_get_variable(channel, "amd_ws_caller_id"));
	if (zstr(cid) && send_cid) {
		/* the number the callee is shown: effective > origination > caller profile */
		cid = switch_channel_get_variable(channel, "effective_caller_id_number");
		if (!valid_cid_value(cid)) cid = switch_channel_get_variable(channel, "origination_caller_id_number");
		if (!valid_cid_value(cid)) cid = switch_channel_get_variable(channel, "caller_id_number");
	}
	if (!valid_cid_value(cid)) cid = NULL;

	/* {"config":{"sample_rate":8000,"VID":..[,"phone":..][,"country_code":..][,"caller_id":..][,extra]}} */
	switch_snprintf(json, jsonlen, "{\"config\":{\"sample_rate\":8000,\"VID\":");
	amd_json_append_str(json, jsonlen, call->vid);
	if (!zstr(phone)) { switch_snprintf(json + strlen(json), jsonlen - strlen(json), ",\"phone\":"); amd_json_append_str(json, jsonlen, phone); }
	if (!zstr(code)) { switch_snprintf(json + strlen(json), jsonlen - strlen(json), ",\"country_code\":"); amd_json_append_str(json, jsonlen, code); }
	if (!zstr(cid)) { switch_snprintf(json + strlen(json), jsonlen - strlen(json), ",\"caller_id\":"); amd_json_append_str(json, jsonlen, cid); }
	if (!zstr(extra)) switch_snprintf(json + strlen(json), jsonlen - strlen(json), ",%s", extra);
	switch_snprintf(json + strlen(json), jsonlen - strlen(json), "}}");

	if (!zstr(phone)) switch_channel_set_variable(channel, "AMDPHONE", phone);
	if (!zstr(code)) switch_channel_set_variable(channel, "AMDCOUNTRYCODE", code);

	switch_copy_string(call->host, c->host, sizeof(call->host));
	call->port = c->port;
	call->mode = c->mode;
	call->playdelay_ms = playdelay;
	if (!zstr(a.playfile)) switch_copy_string(call->playfile, a.playfile, sizeof(call->playfile));

	switch_copy_string(c->tag, switch_channel_get_name(channel), sizeof(c->tag));
	switch_copy_string(c->log_id, switch_core_session_get_uuid(call->session), sizeof(c->log_id));
	c->log = engine_log;
	c->log_ud = NULL;
}

/* ------------------------------------------------------------------------ */
/* results                                                                    */

static void apply_result(amd_call_t *call)
{
	switch_channel_t *channel = call->channel;
	amd_result_t r;
	char buf[64];
	switch_event_t *ev = NULL;

	if (call->applied) return;
	call->applied = 1;
	amd_engine_result(call->engine, &r);

	switch_channel_set_variable(channel, "AMDSTATUS", r.status);
	switch_channel_set_variable(channel, "AMDCAUSE", r.cause);
	switch_snprintf(buf, sizeof(buf), "%ld-%ld-%ld-%ld", r.elapsed_ms, r.audio_ms, r.chunks, r.bytes);
	switch_channel_set_variable(channel, "AMDSTATS", buf);
	switch_channel_set_variable(channel, "AMDRESPONSE", r.response);
	switch_snprintf(buf, sizeof(buf), "%ld", r.elapsed_ms);
	switch_channel_set_variable(channel, "AMDELAPSED", buf);
	/* mod_amd names, so dialplans written for mod_amd keep working */
	switch_channel_set_variable(channel, "amd_result", r.status);
	switch_channel_set_variable(channel, "amd_cause", r.cause);
	switch_channel_set_variable(channel, "amd_ws_source", r.source);
	switch_channel_set_variable(channel, "amd_ws_mode", mode_str(call->mode));
	if (r.connect_ms >= 0) {
		switch_snprintf(buf, sizeof(buf), "%ld", r.connect_ms);
		switch_channel_set_variable(channel, "amd_ws_connect_ms", buf);
	}
	if (r.local_status[0]) {
		switch_channel_set_variable(channel, "amd_ws_local_result", r.local_status);
		switch_channel_set_variable(channel, "amd_ws_local_cause", r.local_cause);
		switch_snprintf(buf, sizeof(buf), "%ld", r.local_ms);
		switch_channel_set_variable(channel, "amd_ws_local_ms", buf);
	}
	if (r.fallback_from[0]) switch_channel_set_variable(channel, "amd_ws_fallback_from", r.fallback_from);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(call->session), SWITCH_LOG_INFO,
					  "AMD_WS: %s vid=%s status=%s cause=%s elapsed=%ld sent=%ld chunks=%ld%s%s%s%s\n",
					  switch_channel_get_name(channel), call->vid, r.status, r.cause, r.elapsed_ms, r.bytes, r.chunks,
					  r.local_status[0] ? " local=" : "", r.local_status[0] ? r.local_status : "",
					  r.local_status[0] ? "/" : "", r.local_status[0] ? r.local_cause : "");

	if (switch_event_create_subclass(&ev, SWITCH_EVENT_CUSTOM, AMD_WS_EVENT) == SWITCH_STATUS_SUCCESS) {
		switch_channel_event_set_data(channel, ev);
		switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMD-Status", r.status);
		switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMD-Cause", r.cause);
		switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMD-Response", r.response);
		switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMD-Source", r.source);
		switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMD-Mode", mode_str(call->mode));
		switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMD-VID", call->vid);
		switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMD-Elapsed", "%ld", r.elapsed_ms);
		switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMD-Stats", "%ld-%ld-%ld-%ld", r.elapsed_ms, r.audio_ms, r.chunks, r.bytes);
		switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMD-Connect-MS", "%ld", r.connect_ms);
		switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMD-First-Audio-MS", "%ld", r.first_audio_ms);
		switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMD-Background", "%s", call->background ? "true" : "false");
		if (r.local_status[0]) {
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMD-Local-Status", r.local_status);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMD-Local-Cause", r.local_cause);
			switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMD-Local-MS", "%ld", r.local_ms);
		}
		if (r.fallback_from[0]) switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMD-Fallback-From", r.fallback_from);
		switch_event_fire(&ev);
	}

	if (call->background && switch_channel_up(channel)) {
		switch_channel_api_on(channel, "api_on_amd_ws_result");
	}
}

/* ------------------------------------------------------------------------ */
/* media bug                                                                  */

static void process_frame(amd_call_t *call, switch_frame_t *f)
{
	int16_t *s = f->data;
	uint32_t n = f->samples ? f->samples : f->datalen / 2 / (call->in_channels ? call->in_channels : 1);
	uint32_t i;
	int cng = 1;
	int16_t mono[SWITCH_RECOMMENDED_BUFFER_SIZE / 2];

	if (!n) return;
	if (call->in_channels > 1) {
		if (n > sizeof(mono) / sizeof(mono[0])) n = sizeof(mono) / sizeof(mono[0]);
		for (i = 0; i < n; i++) mono[i] = s[i * call->in_channels];
		s = mono;
	}
	/* the core fills missing media (CNG) with 0xFF bytes = -1 samples */
	for (i = 0; i < n; i++) {
		if (s[i] != -1) { cng = 0; break; }
	}
	if (call->resampler) {
		switch_resample_process(call->resampler, s, n);
		amd_engine_feed(call->engine, call->resampler->to, call->resampler->to_len, cng);
	} else {
		amd_engine_feed(call->engine, s, n, cng);
	}
}

static void call_end(amd_call_t *call)
{
	switch_channel_set_private(call->channel, AMD_WS_PRIVATE, NULL);
	if (call->resampler) switch_resample_destroy(&call->resampler);
	if (call->engine) {
		amd_engine_release(call->engine);
		call->engine = NULL;
	}
	switch_mutex_lock(globals.mutex);
	globals.active--;
	switch_mutex_unlock(globals.mutex);
	switch_thread_rwlock_unlock(globals.mi->rwlock);
}

static switch_bool_t bug_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	amd_call_t *call = user_data;

	switch (type) {
	case SWITCH_ABC_TYPE_INIT: {
		switch_codec_implementation_t impl = { 0 };
		switch_core_session_get_read_impl(call->session, &impl);
		call->in_rate = impl.actual_samples_per_second ? impl.actual_samples_per_second : 8000;
		call->in_channels = impl.number_of_channels ? impl.number_of_channels : 1;
		if (call->in_rate != 8000) {
			if (switch_resample_create(&call->resampler, call->in_rate, 8000, SWITCH_RECOMMENDED_BUFFER_SIZE,
									   SWITCH_RESAMPLE_QUALITY, 1) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(call->session), SWITCH_LOG_ERROR,
								  "AMD_WS: cannot create %u->8000 Hz resampler\n", call->in_rate);
				return SWITCH_FALSE;
			}
		}
		break;
	}
	case SWITCH_ABC_TYPE_READ: {
		uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
		switch_frame_t frame = { 0 };

		/* Exactly one read per callback: the core calls us once per frame it
		 * buffered, and on an empty buffer switch_core_media_bug_read() does
		 * not fail but returns a synthetic 0xFF-filled frame. */
		frame.data = data;
		frame.buflen = sizeof(data);
		if (switch_core_media_bug_read(bug, &frame, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS && frame.datalen) {
			frame.samples = 0;
			process_frame(call, &frame);
		}
		if (amd_engine_check(call->engine)) {
			if (call->background) {
				apply_result(call);
				return SWITCH_FALSE;      /* prune: CLOSE follows */
			}
			if (call->playing) switch_channel_set_flag(call->channel, CF_BREAK);
		}
		break;
	}
	case SWITCH_ABC_TYPE_CLOSE:
		if (call->background) {
			if (!call->applied) {
				if (call->stopped) amd_engine_finish(call->engine, "NOTSURE", "STOPPED");
				else amd_engine_hangup(call->engine, NULL);
				apply_result(call);
			}
			call_end(call);
		}
		break;
	default:
		break;
	}
	return SWITCH_TRUE;
}

/* ------------------------------------------------------------------------ */
/* start (shared by the application and uuid_amd_ws)                          */

static amd_call_t *call_start(switch_core_session_t *session, const char *data, int background, const char **err)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	amd_call_t *call;
	amd_engine_cfg_t c;
	char json[4096];
	char *dup = NULL;
	int noanswer = 0;
	switch_status_t st;

	if (switch_channel_get_private(channel, AMD_WS_PRIVATE)) {
		*err = "detection already running on this channel";
		return NULL;
	}
	call = switch_core_session_alloc(session, sizeof(*call));
	call->session = session;
	call->channel = channel;
	call->background = background;
	call->t_start = amdws_now_ms();
	if (!zstr(data)) dup = switch_core_session_strdup(session, data);
	build_call(call, dup, &c, json, sizeof(json), &noanswer);

	if (!switch_channel_test_flag(channel, CF_ANSWERED) && !switch_channel_test_flag(channel, CF_EARLY_MEDIA)) {
		if (noanswer || background) {
			*err = "channel has no media (not answered)";
			return NULL;
		}
		if (switch_channel_answer(channel) != SWITCH_STATUS_SUCCESS) {
			*err = "answer failed";
			return NULL;
		}
	}

	/* keep the module loaded while this call uses it */
	switch_thread_rwlock_rdlock(globals.mi->rwlock);
	switch_mutex_lock(globals.mutex);
	globals.active++;
	switch_mutex_unlock(globals.mutex);

	call->engine = amd_engine_start(&c, json);
	if (!call->engine) {
		switch_mutex_lock(globals.mutex);
		globals.active--;
		switch_mutex_unlock(globals.mutex);
		switch_thread_rwlock_unlock(globals.mi->rwlock);
		*err = "out of memory";
		return NULL;
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
					  "AMD_WS: %s vid=%s host=%s:%d mode=%s%s play=%s%s\n", switch_channel_get_name(channel), call->vid,
					  call->host, call->port, mode_str(c.mode), c.local_fallback ? "+fallback" : "",
					  zstr(call->playfile) ? "none" : call->playfile, background ? " (background)" : "");

	switch_channel_set_private(channel, AMD_WS_PRIVATE, call);
	st = switch_core_media_bug_add(session, "amd_ws", NULL, bug_callback, call, 0,
								   SMBF_READ_STREAM | SMBF_NO_PAUSE | SMBF_ONE_ONLY, &call->bug);
	if (st != SWITCH_STATUS_SUCCESS) {
		/* ONE_ONLY: another amd_ws bug exists, or the channel has no media */
		switch_channel_set_private(channel, AMD_WS_PRIVATE, NULL);
		amd_engine_finish(call->engine, "HUMAN", "FATAL_ERROR");
		if (background) {
			amd_engine_release(call->engine);
			call->engine = NULL;
			switch_mutex_lock(globals.mutex);
			globals.active--;
			switch_mutex_unlock(globals.mutex);
			switch_thread_rwlock_unlock(globals.mi->rwlock);
			*err = "cannot attach media bug";
			return NULL;
		}
		call->bug = NULL;
	}
	return call;
}

/* ------------------------------------------------------------------------ */
/* dialplan application                                                       */

static switch_status_t play_break_cb(switch_core_session_t *session, void *input, switch_input_type_t itype, void *buf, unsigned int buflen)
{
	return SWITCH_STATUS_SUCCESS;   /* DTMF does not stop the prompt */
}

SWITCH_STANDARD_APP(amd_ws_app)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	amd_call_t *call;
	const char *err = NULL;
	int played = 0;

	if (!(call = call_start(session, data, 0, &err))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "AMD_WS: %s: %s -> HUMAN/FATAL_ERROR\n",
						  switch_channel_get_name(channel), err ? err : "error");
		switch_channel_set_variable(channel, "AMDSTATUS", "HUMAN");
		switch_channel_set_variable(channel, "AMDCAUSE", "FATAL_ERROR");
		switch_channel_set_variable(channel, "AMDSTATS", "0-0-0-0");
		switch_channel_set_variable(channel, "AMDELAPSED", "0");
		switch_channel_set_variable(channel, "AMDRESPONSE", "");
		switch_channel_set_variable(channel, "amd_result", "HUMAN");
		switch_channel_set_variable(channel, "amd_cause", "FATAL_ERROR");
		return;
	}

	if (call->bug) {
		while (switch_channel_ready(channel)) {
			switch_frame_t *f = NULL;
			switch_status_t st;

			if (amd_engine_check(call->engine)) break;

			if (!zstr(call->playfile) && !played && amdws_now_ms() - call->t_start >= call->playdelay_ms) {
				switch_input_args_t args = { 0 };
				args.input_callback = play_break_cb;
				played = 1;
				call->playing = 1;
				switch_ivr_play_file(session, NULL, call->playfile, &args);
				call->playing = 0;
				switch_channel_clear_flag(channel, CF_BREAK);
				continue;
			}
			st = switch_core_session_read_frame(session, &f, SWITCH_IO_FLAG_NONE, 0);
			if (!SWITCH_READ_ACCEPTABLE(st)) break;
		}
		if (!amd_engine_check(call->engine)) {
			amd_engine_hangup(call->engine, NULL);
		}
		switch_core_media_bug_remove(session, &call->bug);
	}
	apply_result(call);
	call_end(call);
}

/* ------------------------------------------------------------------------ */
/* API                                                                        */

SWITCH_STANDARD_API(uuid_amd_ws_api)
{
	char *mydata = NULL, *argv[3] = { 0 };
	int argc = 0;
	switch_core_session_t *s = NULL;

	if (zstr(cmd) || !(mydata = strdup(cmd))) {
		stream->write_function(stream, "-USAGE: %s\n", UUID_AMD_WS_SYNTAX);
		return SWITCH_STATUS_SUCCESS;
	}
	argc = (int) switch_separate_string(mydata, ' ', argv, 3);
	if (argc < 2) {
		stream->write_function(stream, "-USAGE: %s\n", UUID_AMD_WS_SYNTAX);
		goto done;
	}
	if (!(s = switch_core_session_locate(argv[0]))) {
		stream->write_function(stream, "-ERR no such channel %s\n", argv[0]);
		goto done;
	}
	if (!strcasecmp(argv[1], "start")) {
		const char *err = NULL;
		if (call_start(s, argc > 2 ? argv[2] : NULL, 1, &err)) {
			stream->write_function(stream, "+OK\n");
		} else {
			stream->write_function(stream, "-ERR %s\n", err ? err : "cannot start");
		}
	} else if (!strcasecmp(argv[1], "stop")) {
		switch_channel_t *ch = switch_core_session_get_channel(s);
		amd_call_t *call = switch_channel_get_private(ch, AMD_WS_PRIVATE);
		if (!call || !call->background || !call->bug) {
			stream->write_function(stream, "-ERR no background detection on this channel\n");
		} else {
			call->stopped = 1;
			switch_core_media_bug_remove(s, &call->bug);
			stream->write_function(stream, "+OK\n");
		}
	} else {
		stream->write_function(stream, "-USAGE: %s\n", UUID_AMD_WS_SYNTAX);
	}

done:
	if (s) switch_core_session_rwunlock(s);
	switch_safe_free(mydata);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(amd_ws_api)
{
	if (!zstr(cmd) && !strcasecmp(cmd, "reload")) {
		load_config();
		stream->write_function(stream, "+OK reloaded %s\n", globals.config_file);
		return SWITCH_STATUS_SUCCESS;
	}
	if (zstr(cmd) || !strcasecmp(cmd, "status") || !strcasecmp(cmd, "show")) {
		amd_engine_cfg_t c;
		amd_engine_stats_t st;
		char sched[256] = "";
		int i, active;

		switch_mutex_lock(globals.mutex);
		c = globals.base;
		active = globals.active;
		switch_mutex_unlock(globals.mutex);
		amd_engine_stats(&st);
		for (i = 0; i < c.nschedule; i++) {
			switch_snprintf(sched + strlen(sched), sizeof(sched) - strlen(sched), "%s%d", i ? "," : "", c.schedule[i]);
		}
		stream->write_function(stream,
			"mod_amd_ws %s\n"
			"  endpoint              %s://%s:%d%s\n"
			"  mode                  %s%s\n"
			"  tls_verify            %s (hostname check %s)\n"
			"  timeout_ms            %d\n"
			"  connect_timeout_ms    %d\n"
			"  send_schedule         %s\n"
			"  chunk_bytes           %d\n"
			"  fallback_interval_ms  %d\n"
			"  eof_no_audio_streak   %d (eof_wait_ms %d)\n"
			"  result_grace_ms       %d\n"
			"  local detector        silence_threshold=%u initial_silence=%u greeting=%u after_greeting_silence=%u\n"
			"                        total_analysis_time=%u min_word_length=%u between_words_silence=%u\n"
			"                        maximum_number_of_words=%u maximum_word_length=%u\n"
			"  calls in progress     %d\n"
			"  worker threads        %ld\n"
			"  results               calls=%ld human=%ld machine=%ld notsure=%ld hangup=%ld\n"
			"  errors                connection=%ld processing=%ld server_timeout=%ld noaudio=%ld eof=%ld fallback_used=%ld\n",
			MOD_AMD_WS_VERSION, c.tls ? "wss" : "ws", c.host, c.port, c.path, mode_str(c.mode),
			c.local_fallback ? " + local fallback" : "", c.tls_verify ? "yes" : "no", c.tls_check_hostname ? "on" : "off",
			c.timeout_ms, c.connect_timeout_ms, sched, c.chunk_bytes, c.fallback_interval_ms, c.eof_no_audio_streak,
			c.eof_wait_ms, c.result_grace_ms, c.local.silence_threshold, c.local.initial_silence, c.local.greeting,
			c.local.after_greeting_silence, c.local.total_analysis_time, c.local.min_word_length,
			c.local.between_words_silence, c.local.maximum_number_of_words, c.local.maximum_word_length, active,
			st.workers_alive, st.calls, st.human, st.machine, st.notsure, st.hangup, st.connection_error,
			st.processing_error, st.server_timeout, st.noaudio, st.eof, st.fallback_used);
		return SWITCH_STATUS_SUCCESS;
	}
	stream->write_function(stream, "-USAGE: amd_ws status|reload\n");
	return SWITCH_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* module                                                                     */

#define AMD_WS_DESC \
	"Answering machine detection over WebSocket (amdy.io protocol).\n" \
	"Arguments: " AMD_WS_SYNTAX "\n" \
	"Options: s=wss  d(ms)=playback delay  c(ms)=connect timeout  p(phone)  k(country code)  i(caller id)\n" \
	"         a=answer (default)  A=do not answer  v=trace  L=local detector only  C=compare (ws + local)\n" \
	"         F=local fallback on connection/processing errors  f=no fallback\n" \
	"Sets AMDSTATUS (HUMAN|MACHINE|NOTSURE|HANGUP), AMDCAUSE, AMDSTATS, AMDRESPONSE, AMDELAPSED,\n" \
	"amd_result/amd_cause (mod_amd names) and fires CUSTOM amd_ws::result."

SWITCH_MODULE_LOAD_FUNCTION(mod_amd_ws_load)
{
	switch_application_interface_t *app_interface;
	switch_api_interface_t *api_interface;

	memset(&globals, 0, sizeof(globals));
	globals.pool = pool;
	switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, pool);
	switch_copy_string(globals.config_file, "amd_ws.conf", sizeof(globals.config_file));

	if (switch_event_reserve_subclass(AMD_WS_EVENT) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "amd_ws: cannot register event subclass %s\n", AMD_WS_EVENT);
		return SWITCH_STATUS_TERM;
	}

	amd_engine_global_init();
	load_config();

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	globals.mi = *module_interface;

	SWITCH_ADD_APP(app_interface, "amd_ws", "Answering machine detection over WebSocket", AMD_WS_DESC, amd_ws_app,
				   AMD_WS_SYNTAX, SAF_NONE);
	SWITCH_ADD_API(api_interface, "uuid_amd_ws", "Start/stop background AMD on a channel", uuid_amd_ws_api, UUID_AMD_WS_SYNTAX);
	SWITCH_ADD_API(api_interface, "amd_ws", "mod_amd_ws settings and counters", amd_ws_api, "status|reload");
	switch_console_set_complete("add uuid_amd_ws ::console::list_uuid start");
	switch_console_set_complete("add uuid_amd_ws ::console::list_uuid stop");
	switch_console_set_complete("add amd_ws status");
	switch_console_set_complete("add amd_ws reload");

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_amd_ws %s loaded (%s:%d, mode %s)\n", MOD_AMD_WS_VERSION,
					  globals.base.host, globals.base.port, mode_str(globals.base.mode));
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_amd_ws_shutdown)
{
	long alive = amd_engine_wait_idle(10000);

	switch_console_set_complete("del uuid_amd_ws");
	switch_console_set_complete("del amd_ws");
	switch_event_free_subclass(AMD_WS_EVENT);
	if (alive > 0) {
		/* a worker is still inside getaddrinfo(); unmapping the code would crash it */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "amd_ws: %ld worker thread(s) still running; module stays mapped until restart\n", alive);
		return SWITCH_STATUS_NOUNLOAD;
	}
	return SWITCH_STATUS_SUCCESS;
}
