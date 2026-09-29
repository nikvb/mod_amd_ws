/*
 * amd_engine.h -- one answering-machine-detection run (one call).
 *
 * The engine is independent of FreeSWITCH: the module feeds it 8 kHz slin
 * frames from a media bug and asks it for the result; a detached worker
 * thread owns the WebSocket, the send schedule, the reply classifier and
 * every timeout.  The protocol is that of the amdy.io EAGI client amd.py
 * (July 2026), identical to app_amd_ws for Asterisk; see docs/protocol.md.
 *
 * Threading: amd_engine_feed / _check / _hangup / _result / _release are
 * called from the channel side (media-bug callback or the application
 * thread), one thread at a time per engine.  The worker never touches the
 * channel; everything it shares with the channel side is behind e->mutex.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 1.1.  See LICENSE.
 */
#ifndef MOD_AMD_WS_ENGINE_H
#define MOD_AMD_WS_ENGINE_H

#include <stdint.h>
#include <stddef.h>
#include "local_amd.h"

#define AMD_MAX_SCHEDULE 16

typedef enum {
	AMD_MODE_WS = 0,      /* stream to the service; its verdict is the result      */
	AMD_MODE_LOCAL,       /* energy detector only (mod_amd behaviour), no network  */
	AMD_MODE_COMPARE      /* stream; the local detector runs in parallel for stats  */
} amd_mode_t;

typedef enum { AMD_LOG_DEBUG = 0, AMD_LOG_INFO, AMD_LOG_NOTICE, AMD_LOG_WARNING, AMD_LOG_ERROR } amd_log_level_t;

typedef void (*amd_log_fn)(void *ud, amd_log_level_t level, const char *msg);

typedef struct {
	char host[256];
	int port;
	char path[128];
	int tls, tls_verify, tls_check_hostname;
	char tls_cafile[512];

	int timeout_ms;             /* detection window from the first captured frame */
	int connect_timeout_ms;
	int result_grace_ms;
	int schedule[AMD_MAX_SCHEDULE];
	int nschedule;
	int chunk_bytes;
	int fallback_interval_ms;
	int eof_no_audio_streak;
	int eof_wait_ms;

	amd_mode_t mode;
	int local_fallback;         /* on CONNECTION/PROCESSING error use the local verdict */
	int compare_wait_local;     /* compare mode: report done only once the local
	                               detector has decided too (the ws result and its
	                               timing are unchanged) */
	local_amd_params_t local;

	int trace;                  /* per-event lines at INFO instead of DEBUG */
	char tag[128];              /* channel name for log lines */
	char log_id[64];            /* passed to log() when log_ud is NULL (e.g. the call uuid);
	                               owned by the engine, so valid for the worker's lifetime */
	amd_log_fn log;
	void *log_ud;
} amd_engine_cfg_t;

typedef struct {
	char status[16];            /* HUMAN MACHINE NOTSURE HANGUP */
	char cause[256];
	char response[256];         /* last text from the server, sanitised */
	long elapsed_ms;            /* from the first captured frame (or start) */
	long audio_ms, chunks, bytes;
	long connect_ms;            /* -1 = never connected */
	long first_audio_ms;        /* first captured frame, from start; -1 = none */
	char source[16];            /* "ws", "local", "fallback", "hangup", "timeout", "error" */
	char fallback_from[32];     /* ws error that triggered the local fallback */
	char local_status[16];      /* PENDING HUMAN MACHINE NOTSURE, "" when not run */
	char local_cause[32];
	long local_ms;              /* local decision, ms of audio analysed; -1 = none */
} amd_result_t;

typedef struct amd_engine amd_engine_t;

void amd_engine_global_init(void);

/* Create and (unless mode is LOCAL) start the worker.  config_json is the
 * complete config TEXT frame; copied.  Returns NULL on allocation failure. */
amd_engine_t *amd_engine_start(const amd_engine_cfg_t *cfg, const char *config_json);

/* Channel side.  is_cng: the frame is comfort-noise fill (no media arrived);
 * it feeds the local detector as silence but is not audio for the service. */
void amd_engine_feed(amd_engine_t *e, const int16_t *s, uint32_t nsamples, int is_cng);

/* Evaluate the channel-side rules (local verdict, fallback, connect
 * watchdog).  Returns 1 once a final result exists. */
int amd_engine_check(amd_engine_t *e);

/* The channel hung up (or detection was stopped): HANGUP/HANGUP unless a
 * result already exists.  cause NULL = "HANGUP". */
void amd_engine_hangup(amd_engine_t *e, const char *cause);

/* End with an explicit status/cause unless a result already exists
 * (e.g. NOTSURE/STOPPED, HUMAN/FATAL_ERROR). */
void amd_engine_finish(amd_engine_t *e, const char *status, const char *cause);

/* Copy the result (valid once amd_engine_check() returned 1 or after hangup). */
void amd_engine_result(amd_engine_t *e, amd_result_t *r);

/* Channel side is done: stop the worker (it sends {"eof":1} + CLOSE 1000
 * and exits on its own) and drop the channel's reference.  Never blocks. */
void amd_engine_release(amd_engine_t *e);

/* Counters for "amd_ws status". */
typedef struct {
	long workers_alive;
	long calls, human, machine, notsure, hangup;
	long connection_error, processing_error, server_timeout, noaudio, eof;
	long fallback_used;
} amd_engine_stats_t;
void amd_engine_stats(amd_engine_stats_t *s);

/* Wait up to max_ms for every worker to exit (module unload). Returns the
 * number still alive. */
long amd_engine_wait_idle(int max_ms);

/* amd.py classifier: 1 = HUMAN, 2 = MACHINE, 0 = acknowledgement. */
int amd_classify(const char *text);

/* JSON string escaping for the config frame: valid UTF-8 passes, invalid
 * bytes become '?', control characters are escaped.  Appends to dst. */
void amd_json_append_str(char *dst, size_t dstlen, const char *s);

#endif
