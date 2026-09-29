/*
 * ws_client.h -- minimal RFC 6455 WebSocket client (ws:// and wss://)
 *
 * Part of mod_amd_ws.  Plain C99 + POSIX + OpenSSL; no FreeSWITCH dependency,
 * so the engine and this client can be tested without a running switch.
 *
 * Every call that can wait takes an absolute deadline (monotonic ms, see
 * amdws_now_ms()) and an optional abort descriptor: when abort_fd becomes
 * readable the call returns WS_ABORTED at once.  Nothing blocks without bound.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 1.1.  See LICENSE.
 */
#ifndef MOD_AMD_WS_CLIENT_H
#define MOD_AMD_WS_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#define WS_OK        0
#define WS_ERR      -1   /* socket/TLS/protocol error              */
#define WS_TIMEOUT  -2   /* deadline passed                        */
#define WS_CLOSED   -3   /* peer sent CLOSE or closed the socket   */
#define WS_ABORTED  -4   /* abort_fd became readable               */

#define WS_OP_CONT   0x0
#define WS_OP_TEXT   0x1
#define WS_OP_BINARY 0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

typedef struct amdws_client amdws_client_t;

typedef struct {
	const char *host;          /* name or IPv4/IPv6 literal (no brackets)  */
	int port;
	const char *path;          /* "/" when NULL                            */
	int tls;                   /* 1 = wss://                               */
	int tls_verify;            /* verify the certificate chain             */
	int tls_check_hostname;    /* also check CN/SAN against host           */
	const char *tls_cafile;    /* NULL/"" = system default locations      */
	const char *user_agent;    /* NULL = "mod_amd_ws"                      */
	int abort_fd;              /* -1 = none                                */
} amdws_connect_opts_t;

typedef struct {
	int opcode;                /* WS_OP_TEXT or WS_OP_BINARY               */
	char *data;                /* NUL-terminated; owned by the client,     */
	size_t len;                /* valid until the next amdws_read()           */
} amdws_msg_t;

/* Monotonic clock in milliseconds. */
int64_t amdws_now_ms(void);

/* One-time library initialisation (safe to call repeatedly). */
void amdws_global_init(void);

/* DNS + TCP (+ TLS) + HTTP upgrade, all bounded by deadline_ms.
 * On failure *out is NULL and err holds a one-line reason. */
int amdws_connect(amdws_client_t **out, const amdws_connect_opts_t *opts, int64_t deadline_ms,
			   char *err, size_t errlen);

/* Descriptor to poll() for POLLIN. */
int amdws_fd(const amdws_client_t *ws);

/* 1 when a complete frame (or TLS plaintext) is already buffered, so the
 * caller must call amdws_read() without waiting for the descriptor. */
int amdws_pending(const amdws_client_t *ws);

/* Send one frame (masked, FIN set).  Waits for socket space until deadline. */
int amdws_send(amdws_client_t *ws, int opcode, const void *data, size_t len, int64_t deadline_ms);

/* Non-blocking: pull whatever the socket has, then return
 *   1          one complete TEXT/BINARY message in *msg (fragments reassembled)
 *   0          nothing complete yet
 *   WS_CLOSED  the peer sent CLOSE (answered) or closed the connection
 *   WS_ERR     protocol or socket error
 * PING is answered with PONG internally; PONG is ignored. */
int amdws_read(amdws_client_t *ws, amdws_msg_t *msg);

/* Close status the peer sent (0 if none). */
int amdws_peer_close_code(const amdws_client_t *ws);

/* Best effort: send CLOSE(code) if the connection is still open (bounded by
 * a short internal deadline), shut down TLS, close the socket, free. */
void amdws_close(amdws_client_t *ws, int code);

/* Human-readable text for a WS_* code. */
const char *amdws_strerror(int rc);

#endif
