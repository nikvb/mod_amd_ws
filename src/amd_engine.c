/*
 * amd_engine.c -- see amd_engine.h.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 1.1.  See LICENSE.
 */
#define _GNU_SOURCE
#include "amd_engine.h"
#include "ws_client.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_FRAME_SEND 160000        /* only split pathological backlogs (10 s) */
#define EXIT_SEND_WAIT_MS 300        /* {"eof":1} on the way out */
#define WATCHDOG_SLACK_MS 500        /* channel side declares CONNECTION_ERROR */
#define MAX_POLL_MS 100

typedef enum { ST_PENDING = 0, ST_DONE } state_t;

struct amd_engine {
	pthread_mutex_t mutex;
	int refs;

	amd_engine_cfg_t cfg;
	char *config_json;
	int wake[2];                 /* self-pipe: [0] polled by the worker */

	int64_t t_start;             /* monotonic ms */
	int64_t t_first_audio;       /* 0 = none yet */
	int64_t t_connected;         /* 0 = not (yet) */
	int stop;                    /* channel side released the engine */
	int worker_running;

	/* audio captured, not yet sent (8 kHz slin) */
	unsigned char *buf;
	size_t buf_len, buf_cap;
	uint64_t captured_bytes;     /* ever captured (real audio only) */

	/* ws failure seen by the worker while local_fallback is on */
	char ws_failed[32];

	local_amd_t local;
	int local_on;

	state_t state;
	amd_result_t res;

	/* worker-owned counters, copied into res */
	long chunks, bytes;
	char response[256];
};

/* ------------------------------------------------------------------------ */
/* global counters                                                           */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static amd_engine_stats_t g_stats;

void amd_engine_global_init(void)
{
	amdws_global_init();
}

void amd_engine_stats(amd_engine_stats_t *s)
{
	pthread_mutex_lock(&g_lock);
	*s = g_stats;
	pthread_mutex_unlock(&g_lock);
}

long amd_engine_wait_idle(int max_ms)
{
	int64_t deadline = amdws_now_ms() + max_ms;
	long alive;
	pthread_mutex_lock(&g_lock);
	while (g_stats.workers_alive > 0 && amdws_now_ms() < deadline) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 100 * 1000000L;
		if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
		pthread_cond_timedwait(&g_cond, &g_lock, &ts);
	}
	alive = g_stats.workers_alive;
	pthread_mutex_unlock(&g_lock);
	return alive;
}

static void count_result(const amd_result_t *r)
{
	pthread_mutex_lock(&g_lock);
	g_stats.calls++;
	if (!strcmp(r->status, "HUMAN")) g_stats.human++;
	else if (!strcmp(r->status, "MACHINE")) g_stats.machine++;
	else if (!strcmp(r->status, "NOTSURE")) g_stats.notsure++;
	else if (!strcmp(r->status, "HANGUP")) g_stats.hangup++;
	if (!strcmp(r->cause, "CONNECTION_ERROR")) g_stats.connection_error++;
	else if (!strcmp(r->cause, "PROCESSING_ERROR")) g_stats.processing_error++;
	else if (!strcmp(r->cause, "SERVER_TIMEOUT")) g_stats.server_timeout++;
	else if (!strncmp(r->cause, "NOAUDIODATA", 11)) g_stats.noaudio++;
	else if (!strncmp(r->cause, "EOF_", 4)) g_stats.eof++;
	if (r->fallback_from[0]) g_stats.fallback_used++;
	pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------------------ */
/* helpers                                                                    */

static void elog(amd_engine_t *e, amd_log_level_t lvl, const char *fmt, ...)
{
	char line[600];
	int n;
	va_list ap;
	if (!e->cfg.log) return;
	n = snprintf(line, sizeof(line), "AMD_WS: %s +%lldms ", e->cfg.tag, (long long) (amdws_now_ms() - e->t_start));
	if (n < 0) n = 0;
	va_start(ap, fmt);
	vsnprintf(line + n, sizeof(line) - (size_t) n, fmt, ap);
	va_end(ap);
	e->cfg.log(e->cfg.log_ud ? e->cfg.log_ud : (void *) e->cfg.log_id, lvl, line);
}

/* trace lines: INFO with trace=yes, else DEBUG */
#define TRACE(e, ...) elog((e), (e)->cfg.trace ? AMD_LOG_INFO : AMD_LOG_DEBUG, __VA_ARGS__)

static void sanitize(char *dst, size_t dstlen, const char *src, size_t srclen)
{
	size_t i, o = 0;
	for (i = 0; i < srclen && o + 1 < dstlen; i++) {
		unsigned char c = (unsigned char) src[i];
		dst[o++] = (c >= 0x20 && c < 0x7F) ? (char) c : ' ';
	}
	dst[o] = 0;
	/* trim */
	while (o && dst[o - 1] == ' ') dst[--o] = 0;
}

/* 'AMD' not followed by 'Y' */
static int has_amd(const char *t)
{
	const char *p = t;
	while ((p = strstr(p, "AMD"))) {
		if (p[3] != 'Y') return 1;
		p += 3;
	}
	return 0;
}

int amd_classify(const char *text)
{
	if (!text) return 0;
	if (strstr(text, "HUMAN")) return 1;
	if (has_amd(text) || strstr(text, "MACHINE")) return 2;
	return 0;
}

void amd_json_append_str(char *dst, size_t dstlen, const char *s)
{
	size_t o = strlen(dst);
	const unsigned char *p = (const unsigned char *) s;

#define PUT(ch) do { if (o + 1 < dstlen) dst[o++] = (char) (ch); } while (0)
	PUT('"');
	while (p && *p) {
		unsigned char c = *p;
		int len = 0, k, ok = 1;
		if (c == '"' || c == '\\') { PUT('\\'); PUT(c); p++; continue; }
		if (c < 0x20) {
			char esc[8];
			snprintf(esc, sizeof(esc), "\\u%04x", c);
			for (k = 0; esc[k]; k++) PUT(esc[k]);
			p++;
			continue;
		}
		if (c < 0x80) { PUT(c); p++; continue; }
		if ((c & 0xE0) == 0xC0 && c >= 0xC2) len = 2;
		else if ((c & 0xF0) == 0xE0) len = 3;
		else if ((c & 0xF8) == 0xF0 && c <= 0xF4) len = 4;
		else ok = 0;
		for (k = 1; ok && k < len; k++) {
			if ((p[k] & 0xC0) != 0x80) ok = 0;
		}
		if (ok && len == 3) {
			if (c == 0xE0 && p[1] < 0xA0) ok = 0;            /* overlong */
			if (c == 0xED && p[1] >= 0xA0) ok = 0;           /* surrogates */
		}
		if (ok && len == 4) {
			if (c == 0xF0 && p[1] < 0x90) ok = 0;
			if (c == 0xF4 && p[1] >= 0x90) ok = 0;
		}
		if (!ok) { PUT('?'); p++; continue; }
		if (o + (size_t) len + 2 >= dstlen) break;           /* never cut a sequence */
		for (k = 0; k < len; k++) PUT(p[k]);
		p += len;
	}
	PUT('"');
	dst[o] = 0;
#undef PUT
}

static long ms_since(int64_t t0, int64_t now)
{
	return t0 ? (long) (now - t0) : -1;
}

/* Final result; first writer wins.  Caller holds e->mutex. */
static void set_result_locked(amd_engine_t *e, const char *status, const char *cause, const char *source)
{
	int64_t now = amdws_now_ms();
	amd_result_t *r = &e->res;

	if (e->state == ST_DONE) return;
	e->state = ST_DONE;

	snprintf(r->status, sizeof(r->status), "%s", status);
	snprintf(r->cause, sizeof(r->cause), "%s", cause);
	snprintf(r->source, sizeof(r->source), "%s", source);
	snprintf(r->response, sizeof(r->response), "%s", e->response);
	r->elapsed_ms = (long) (now - (e->t_first_audio ? e->t_first_audio : e->t_start));
	r->bytes = e->bytes;
	r->chunks = e->chunks;
	r->audio_ms = e->bytes / 16;
	r->connect_ms = ms_since(e->t_start, e->t_connected ? e->t_connected : 0);
	if (!e->t_connected) r->connect_ms = -1;
	r->first_audio_ms = e->t_first_audio ? (long) (e->t_first_audio - e->t_start) : -1;
	if (e->local_on) {
		snprintf(r->local_status, sizeof(r->local_status), "%s", local_amd_status_str(e->local.verdict));
		snprintf(r->local_cause, sizeof(r->local_cause), "%s", e->local.cause ? e->local.cause : "");
		r->local_ms = e->local.verdict != LOCAL_AMD_PENDING ? (long) e->local.decided_ms : -1;
	} else {
		r->local_ms = -1;
	}
	if (e->ws_failed[0] && !strcmp(source, "fallback")) {
		snprintf(r->fallback_from, sizeof(r->fallback_from), "%s", e->ws_failed);
	}
	count_result(r);
}

static void set_result(amd_engine_t *e, const char *status, const char *cause, const char *source)
{
	pthread_mutex_lock(&e->mutex);
	set_result_locked(e, status, cause, source);
	pthread_mutex_unlock(&e->mutex);
}

/* A WebSocket failure: final HUMAN/<cause>, unless local_fallback defers it. */
static void ws_failure(amd_engine_t *e, const char *cause)
{
	pthread_mutex_lock(&e->mutex);
	if (e->state != ST_DONE) {
		if (e->cfg.local_fallback && e->local_on) {
			if (!e->ws_failed[0]) snprintf(e->ws_failed, sizeof(e->ws_failed), "%s", cause);
		} else {
			set_result_locked(e, "HUMAN", cause, "error");
		}
	}
	pthread_mutex_unlock(&e->mutex);
}

static void engine_unref(amd_engine_t *e)
{
	int last;
	pthread_mutex_lock(&e->mutex);
	last = (--e->refs == 0);
	pthread_mutex_unlock(&e->mutex);
	if (!last) return;
	if (e->wake[0] >= 0) close(e->wake[0]);
	if (e->wake[1] >= 0) close(e->wake[1]);
	pthread_mutex_destroy(&e->mutex);
	free(e->buf);
	free(e->config_json);
	free(e);
}

/* ------------------------------------------------------------------------ */
/* worker                                                                     */

static int worker_should_stop(amd_engine_t *e)
{
	int s;
	pthread_mutex_lock(&e->mutex);
	s = e->stop || e->state == ST_DONE || e->ws_failed[0];
	pthread_mutex_unlock(&e->mutex);
	return s;
}

/* Take everything buffered.  Returns malloc'd copy (or NULL) and length. */
static unsigned char *take_audio(amd_engine_t *e, size_t *len)
{
	unsigned char *out = NULL;
	pthread_mutex_lock(&e->mutex);
	*len = e->buf_len;
	if (e->buf_len) {
		out = e->buf;
		e->buf = NULL;
		e->buf_len = e->buf_cap = 0;
	}
	pthread_mutex_unlock(&e->mutex);
	return out;
}

static int send_audio(amd_engine_t *e, amdws_client_t *ws, const unsigned char *a, size_t len, int64_t deadline)
{
	size_t off = 0;
	while (off < len) {
		size_t n = len - off > MAX_FRAME_SEND ? MAX_FRAME_SEND : len - off;
		int rc = amdws_send(ws, WS_OP_BINARY, a + off, n, deadline);
		if (rc != WS_OK) return rc;
		pthread_mutex_lock(&e->mutex);
		e->chunks++;
		e->bytes += (long) n;
		pthread_mutex_unlock(&e->mutex);
		TRACE(e, "sent chunk %ld: %zu bytes", e->chunks, n);
		off += n;
	}
	return WS_OK;
}

static void store_response(amd_engine_t *e, const char *text, size_t len)
{
	pthread_mutex_lock(&e->mutex);
	sanitize(e->response, sizeof(e->response), text, len);
	pthread_mutex_unlock(&e->mutex);
}

/* Drain every complete message.  Returns 1 = final result set, 0 = none,
 * -1 = connection failed (result or fallback recorded). */
static int drain_replies(amd_engine_t *e, amdws_client_t *ws)
{
	amdws_msg_t m;
	int rc, n = 0;
	while (n++ < 64 && (rc = amdws_read(ws, &m)) != 0) {
		if (rc < 0) {
			TRACE(e, "connection %s (peer close code %d)", amdws_strerror(rc), amdws_peer_close_code(ws));
			ws_failure(e, "PROCESSING_ERROR");
			return -1;
		}
		if (m.opcode != WS_OP_TEXT) continue;
		store_response(e, m.data, m.len);
		switch (amd_classify(m.data)) {
		case 1:
			TRACE(e, "reply '%s' -> HUMAN", e->response);
			set_result(e, "HUMAN", "HUMAN", "ws");
			return 1;
		case 2: {
			char cause[256];
			sanitize(cause, sizeof(cause), m.data, m.len);
			TRACE(e, "reply '%s' -> MACHINE", cause);
			set_result(e, "MACHINE", cause[0] ? cause : "MACHINE", "ws");
			return 1;
		}
		default:
			TRACE(e, "reply '%s' (ack)", e->response);
		}
	}
	return 0;
}

/* Wait for the socket, the wake pipe or wait_ms. Returns 1 if the socket is readable. */
static int wait_io(amd_engine_t *e, amdws_client_t *ws, int wait_ms)
{
	struct pollfd p[2];
	int rc;
	if (ws && amdws_pending(ws)) return 1;
	if (wait_ms < 0) wait_ms = 0;
	if (wait_ms > MAX_POLL_MS) wait_ms = MAX_POLL_MS;
	p[0].fd = e->wake[0]; p[0].events = POLLIN; p[0].revents = 0;
	p[1].fd = ws ? amdws_fd(ws) : -1; p[1].events = POLLIN; p[1].revents = 0;
	rc = poll(p, ws ? 2 : 1, wait_ms);
	if (rc > 0 && (p[0].revents & POLLIN)) {
		char b[64];
		while (read(e->wake[0], b, sizeof(b)) > 0) { }
	}
	return rc > 0 && ws && p[1].revents != 0;
}

/* EOF finalisation: {"eof":1}, one reply within eof_wait_ms. */
static void eof_finalise(amd_engine_t *e, amdws_client_t *ws)
{
	int64_t deadline = amdws_now_ms() + e->cfg.eof_wait_ms;
	amdws_msg_t m;

	TRACE(e, "no audio for %d marks: sending {\"eof\":1}", e->cfg.eof_no_audio_streak);
	if (amdws_send(ws, WS_OP_TEXT, "{\"eof\":1}", 9, deadline) != WS_OK) {
		set_result(e, "NOTSURE", "EOF_ERROR", "ws");
		return;
	}
	for (;;) {
		int rc;
		int64_t left = deadline - amdws_now_ms();
		pthread_mutex_lock(&e->mutex);
		rc = e->stop || e->state == ST_DONE;
		pthread_mutex_unlock(&e->mutex);
		if (rc) return;
		if (left <= 0) {
			TRACE(e, "no reply to {\"eof\":1} within %d ms", e->cfg.eof_wait_ms);
			set_result(e, "NOTSURE", "EOF_ERROR", "ws");
			return;
		}
		wait_io(e, ws, (int) left);
		rc = amdws_read(ws, &m);
		if (rc < 0) {
			set_result(e, "NOTSURE", "EOF_ERROR", "ws");
			return;
		}
		if (rc == 1 && m.opcode == WS_OP_TEXT) {
			char cause[256];
			store_response(e, m.data, m.len);
			sanitize(cause, sizeof(cause), m.data, m.len);
			switch (amd_classify(m.data)) {
			case 1: set_result(e, "HUMAN", "HUMAN", "ws"); break;
			case 2: set_result(e, "MACHINE", cause[0] ? cause : "MACHINE", "ws"); break;
			default: set_result(e, "NOTSURE", "EOF_INCONCLUSIVE", "ws");
			}
			TRACE(e, "reply to {\"eof\":1}: '%s'", cause);
			return;
		}
	}
}

static void *worker_main(void *arg)
{
	amd_engine_t *e = arg;
	amd_engine_cfg_t *c = &e->cfg;
	amdws_client_t *ws = NULL;
	amdws_connect_opts_t o;
	char err[256];
	int rc, next_mark = 0, streak = 0;
	int64_t connect_deadline, last_send = 0;

	memset(&o, 0, sizeof(o));
	o.host = c->host;
	o.port = c->port;
	o.path = c->path[0] ? c->path : "/";
	o.tls = c->tls;
	o.tls_verify = c->tls_verify;
	o.tls_check_hostname = c->tls_check_hostname;
	o.tls_cafile = c->tls_cafile;
	o.abort_fd = e->wake[0];

	connect_deadline = e->t_start + (c->connect_timeout_ms < c->timeout_ms ? c->connect_timeout_ms : c->timeout_ms);
	TRACE(e, "connecting to %s://%s:%d%s", c->tls ? "wss" : "ws", c->host, c->port, o.path);
	rc = amdws_connect(&ws, &o, connect_deadline, err, sizeof(err));
	if (rc != WS_OK) {
		if (rc != WS_ABORTED) {
			elog(e, AMD_LOG_WARNING, "connect to %s:%d failed: %s -> CONNECTION_ERROR", c->host, c->port, err);
		}
		ws_failure(e, "CONNECTION_ERROR");
		goto out;
	}
	pthread_mutex_lock(&e->mutex);
	e->t_connected = amdws_now_ms();
	pthread_mutex_unlock(&e->mutex);
	TRACE(e, "connected in %lld ms", (long long) (e->t_connected - e->t_start));

	if (amdws_send(ws, WS_OP_TEXT, e->config_json, strlen(e->config_json), amdws_now_ms() + 2000) != WS_OK) {
		ws_failure(e, "PROCESSING_ERROR");
		goto out;
	}
	TRACE(e, "config %s", e->config_json);

	for (;;) {
		int64_t now = amdws_now_ms(), first, deadline, next_event;
		size_t pending;
		uint64_t captured;

		if (worker_should_stop(e)) break;

		pthread_mutex_lock(&e->mutex);
		first = e->t_first_audio;
		pending = e->buf_len;
		captured = e->captured_bytes;
		pthread_mutex_unlock(&e->mutex);

		deadline = (first ? first : e->t_start) + c->timeout_ms;
		next_event = deadline;

		if (first) {
			int64_t el = now - first;
			/* schedule marks */
			while (next_mark < c->nschedule && el >= c->schedule[next_mark]) {
				size_t len;
				unsigned char *a = take_audio(e, &len);
				next_mark++;
				if (len) {
					streak = 0;
					rc = send_audio(e, ws, a, len, deadline + 1000);
					free(a);
					last_send = now;
					if (rc != WS_OK) { ws_failure(e, "PROCESSING_ERROR"); goto out; }
				} else {
					streak++;
					TRACE(e, "mark %d ms: no audio (streak %d)", c->schedule[next_mark - 1], streak);
					if (c->eof_no_audio_streak > 0 && streak >= c->eof_no_audio_streak && e->chunks > 0) {
						eof_finalise(e, ws);
						goto out;
					}
				}
			}
			if (next_mark < c->nschedule) {
				int64_t t = first + c->schedule[next_mark];
				if (t < next_event) next_event = t;
			} else if (pending) {
				/* after the schedule: chunk_bytes or fallback interval */
				if ((int64_t) pending >= c->chunk_bytes || (last_send && now - last_send >= c->fallback_interval_ms)
					|| (!last_send && now - first >= c->fallback_interval_ms)) {
					size_t len;
					unsigned char *a = take_audio(e, &len);
					rc = len ? send_audio(e, ws, a, len, deadline + 1000) : WS_OK;
					free(a);
					last_send = now;
					if (rc != WS_OK) { ws_failure(e, "PROCESSING_ERROR"); goto out; }
				} else {
					int64_t t = (last_send ? last_send : first) + c->fallback_interval_ms;
					if (t < next_event) next_event = t;
				}
			}
		}

		/* detection window */
		if (now >= deadline) {
			if (c->result_grace_ms > 0 && captured) {
				int64_t gd = now + c->result_grace_ms;
				size_t len;
				unsigned char *a = take_audio(e, &len);
				if (len) send_audio(e, ws, a, len, gd);
				free(a);
				TRACE(e, "window over, grace %d ms", c->result_grace_ms);
				while (amdws_now_ms() < gd && !worker_should_stop(e)) {
					if (wait_io(e, ws, (int) (gd - amdws_now_ms())) && drain_replies(e, ws) != 0) goto out;
				}
				if (worker_should_stop(e)) goto out;
			}
			if (!captured) {
				char cause[48];
				snprintf(cause, sizeof(cause), "NOAUDIODATA-%d", c->timeout_ms);
				set_result(e, "NOTSURE", cause, "timeout");
			} else {
				set_result(e, "NOTSURE", "SERVER_TIMEOUT", "timeout");
			}
			break;
		}

		if (wait_io(e, ws, (int) (next_event - now))) {
			if (drain_replies(e, ws) != 0) goto out;
		}
	}

out:
	if (ws) {
		/* amd.py cleanup: {"eof":1} then close; ours: CLOSE 1000 */
		amdws_send(ws, WS_OP_TEXT, "{\"eof\":1}", 9, amdws_now_ms() + EXIT_SEND_WAIT_MS);
		amdws_close(ws, 1000);
		TRACE(e, "closed (sent %ld chunks, %ld bytes)", e->chunks, e->bytes);
	}
	pthread_mutex_lock(&e->mutex);
	e->worker_running = 0;
	pthread_mutex_unlock(&e->mutex);
	engine_unref(e);

	pthread_mutex_lock(&g_lock);
	g_stats.workers_alive--;
	pthread_cond_broadcast(&g_cond);
	pthread_mutex_unlock(&g_lock);
	return NULL;
}

/* ------------------------------------------------------------------------ */
/* channel side                                                               */

amd_engine_t *amd_engine_start(const amd_engine_cfg_t *cfg, const char *config_json)
{
	amd_engine_t *e = calloc(1, sizeof(*e));
	pthread_attr_t attr;
	pthread_t tid;

	if (!e) return NULL;
	e->cfg = *cfg;
	e->config_json = strdup(config_json ? config_json : "{}");
	e->wake[0] = e->wake[1] = -1;
	e->refs = 1;
	e->t_start = amdws_now_ms();
	e->res.local_ms = -1;
	pthread_mutex_init(&e->mutex, NULL);
	if (!e->config_json) { engine_unref(e); return NULL; }

	e->local_on = (cfg->mode != AMD_MODE_WS) || cfg->local_fallback;
	if (e->local_on) local_amd_init(&e->local, &cfg->local);

	if (cfg->mode == AMD_MODE_LOCAL) {
		return e;
	}
	if (pipe2(e->wake, O_NONBLOCK | O_CLOEXEC) < 0) {
		elog(e, AMD_LOG_ERROR, "pipe: %s -> FATAL_ERROR", strerror(errno));
		set_result(e, "HUMAN", "FATAL_ERROR", "error");
		return e;
	}

	e->refs = 2;
	e->worker_running = 1;
	pthread_mutex_lock(&g_lock);
	g_stats.workers_alive++;
	pthread_mutex_unlock(&g_lock);

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	if (pthread_create(&tid, &attr, worker_main, e) != 0) {
		pthread_attr_destroy(&attr);
		e->refs = 1;
		e->worker_running = 0;
		pthread_mutex_lock(&g_lock);
		g_stats.workers_alive--;
		pthread_mutex_unlock(&g_lock);
		elog(e, AMD_LOG_ERROR, "cannot start worker thread -> FATAL_ERROR");
		set_result(e, "HUMAN", "FATAL_ERROR", "error");
		return e;
	}
	pthread_attr_destroy(&attr);
	return e;
}

static void wake(amd_engine_t *e)
{
	if (e->wake[1] >= 0) {
		ssize_t r = write(e->wake[1], "x", 1);
		(void) r;
	}
}

void amd_engine_feed(amd_engine_t *e, const int16_t *s, uint32_t n, int is_cng)
{
	int64_t now;
	if (!e || !n) return;
	now = amdws_now_ms();

	pthread_mutex_lock(&e->mutex);
	if (e->state == ST_DONE && !(e->local_on && e->local.verdict == LOCAL_AMD_PENDING)) {
		pthread_mutex_unlock(&e->mutex);
		return;
	}
	if (e->state == ST_DONE) goto local_only;
	if (!is_cng) {
		size_t add = (size_t) n * 2;
		if (!e->t_first_audio) {
			e->t_first_audio = now;
		}
		if (e->cfg.mode != AMD_MODE_LOCAL) {
			if (e->buf_len + add > e->buf_cap) {
				size_t cap = e->buf_cap ? e->buf_cap : 32000;
				unsigned char *nb;
				while (cap < e->buf_len + add) cap *= 2;
				nb = realloc(e->buf, cap);
				if (nb) { e->buf = nb; e->buf_cap = cap; }
			}
			if (e->buf_len + add <= e->buf_cap) {
				memcpy(e->buf + e->buf_len, s, add);
				e->buf_len += add;
			}
		}
		e->captured_bytes += add;
	}
local_only:
	if (e->local_on && e->local.verdict == LOCAL_AMD_PENDING) {
		if (is_cng) {
			int16_t z[960];
			uint32_t left = n;
			memset(z, 0, sizeof(z));
			while (left && e->local.verdict == LOCAL_AMD_PENDING) {
				uint32_t k = left > 960 ? 960 : left;
				local_amd_feed(&e->local, z, k);
				left -= k;
			}
		} else {
			local_amd_feed(&e->local, s, n);
		}
		if (e->local.verdict != LOCAL_AMD_PENDING) {
			TRACE(e, "local detector: %s/%s after %u ms of audio",
				  local_amd_status_str(e->local.verdict), e->local.cause, e->local.decided_ms);
		}
	}
	pthread_mutex_unlock(&e->mutex);
}

int amd_engine_check(amd_engine_t *e)
{
	int done;
	int64_t now = amdws_now_ms();

	pthread_mutex_lock(&e->mutex);
	if (e->state != ST_DONE) {
		int64_t deadline = (e->t_first_audio ? e->t_first_audio : e->t_start) + e->cfg.timeout_ms;

		if (e->cfg.mode == AMD_MODE_LOCAL) {
			if (e->local.verdict != LOCAL_AMD_PENDING) {
				set_result_locked(e, local_amd_status_str(e->local.verdict), e->local.cause, "local");
			} else if (now >= deadline) {
				char cause[48];
				if (!e->captured_bytes) snprintf(cause, sizeof(cause), "NOAUDIODATA-%d", e->cfg.timeout_ms);
				else snprintf(cause, sizeof(cause), "TOOLONG");
				set_result_locked(e, "NOTSURE", cause, "timeout");
			}
		} else {
			/* connect watchdog: covers a worker stuck in DNS */
			int64_t cd = e->t_start + (e->cfg.connect_timeout_ms < e->cfg.timeout_ms ? e->cfg.connect_timeout_ms : e->cfg.timeout_ms);
			if (!e->t_connected && !e->ws_failed[0] && now >= cd + WATCHDOG_SLACK_MS) {
				if (e->cfg.local_fallback && e->local_on) {
					snprintf(e->ws_failed, sizeof(e->ws_failed), "CONNECTION_ERROR");
				} else {
					set_result_locked(e, "HUMAN", "CONNECTION_ERROR", "error");
				}
			}
			/* local fallback after a ws failure */
			if (e->state != ST_DONE && e->ws_failed[0]) {
				if (e->local.verdict != LOCAL_AMD_PENDING) {
					set_result_locked(e, local_amd_status_str(e->local.verdict), e->local.cause, "fallback");
				} else if (now >= deadline) {
					set_result_locked(e, "HUMAN", e->ws_failed, "error");
				}
			}
			/* a worker that died without a result (cannot happen; defensive) */
			if (e->state != ST_DONE && !e->worker_running && !e->ws_failed[0]) {
				set_result_locked(e, "HUMAN", "PROCESSING_ERROR", "error");
			}
		}
	}
	done = e->state == ST_DONE;
	if (done && e->cfg.mode == AMD_MODE_COMPARE && e->cfg.compare_wait_local && e->local_on
		&& e->local.verdict == LOCAL_AMD_PENDING && strcmp(e->res.status, "HANGUP")) {
		/* keep feeding the local detector; bounded by the detection window */
		int64_t deadline = (e->t_first_audio ? e->t_first_audio : e->t_start) + e->cfg.timeout_ms;
		if (now < deadline) done = 0;
	}
	pthread_mutex_unlock(&e->mutex);
	if (done) wake(e);
	return done;
}

void amd_engine_hangup(amd_engine_t *e, const char *cause)
{
	pthread_mutex_lock(&e->mutex);
	set_result_locked(e, "HANGUP", cause ? cause : "HANGUP", "hangup");
	pthread_mutex_unlock(&e->mutex);
	wake(e);
}

void amd_engine_finish(amd_engine_t *e, const char *status, const char *cause)
{
	pthread_mutex_lock(&e->mutex);
	set_result_locked(e, status, cause, "error");
	pthread_mutex_unlock(&e->mutex);
	wake(e);
}

void amd_engine_result(amd_engine_t *e, amd_result_t *r)
{
	pthread_mutex_lock(&e->mutex);
	*r = e->res;
	if (e->local_on) {   /* the local detector may have decided after the result */
		snprintf(r->local_status, sizeof(r->local_status), "%s", local_amd_status_str(e->local.verdict));
		snprintf(r->local_cause, sizeof(r->local_cause), "%s", e->local.cause ? e->local.cause : "");
		r->local_ms = e->local.verdict != LOCAL_AMD_PENDING ? (long) e->local.decided_ms : -1;
	}
	pthread_mutex_unlock(&e->mutex);
}

void amd_engine_release(amd_engine_t *e)
{
	if (!e) return;
	pthread_mutex_lock(&e->mutex);
	e->stop = 1;
	pthread_mutex_unlock(&e->mutex);
	wake(e);
	engine_unref(e);
}
