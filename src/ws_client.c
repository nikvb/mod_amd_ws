/*
 * ws_client.c -- minimal RFC 6455 WebSocket client for mod_amd_ws.
 *
 * Non-blocking sockets throughout; every wait is a poll() bounded by the
 * caller's deadline and interruptible through abort_fd.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 1.1.  See LICENSE.
 */
#define _GNU_SOURCE
#include "ws_client.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_MAX_HANDSHAKE 8192
#define WS_MAX_FRAME (4 * 1024 * 1024)     /* server replies are small text */
#define WS_RX_CHUNK 16384
#define WS_CLOSE_WAIT_MS 200

struct amdws_client {
	int fd;
	SSL_CTX *ctx;
	SSL *ssl;
	int abort_fd;
	int closed;            /* CLOSE sent or received, or socket dead */
	int close_sent;
	int peer_close_code;

	unsigned char *rx;     /* raw bytes received, not yet parsed */
	size_t rx_len, rx_cap;

	char *msg;             /* reassembled message (fragments) */
	size_t msg_len, msg_cap;
	int msg_opcode;        /* opcode of the message being assembled, 0 = none */

	char *out;             /* last returned message (owned) */
	size_t out_cap;
};

/* ------------------------------------------------------------------------ */

int64_t amdws_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static pthread_once_t amdws_once = PTHREAD_ONCE_INIT;

static void amdws_do_init(void)
{
#if OPENSSL_VERSION_NUMBER < 0x10100000L
	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();
#else
	OPENSSL_init_ssl(0, NULL);
#endif
}

void amdws_global_init(void)
{
	pthread_once(&amdws_once, amdws_do_init);
}

const char *amdws_strerror(int rc)
{
	switch (rc) {
	case WS_OK: return "ok";
	case WS_ERR: return "error";
	case WS_TIMEOUT: return "timeout";
	case WS_CLOSED: return "closed by peer";
	case WS_ABORTED: return "aborted";
	default: return "unknown";
	}
}

static void set_err(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
	if (err && errlen) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(err, errlen, fmt, ap);
		va_end(ap);
	}
}

static int set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl < 0) return -1;
	return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Wait for fd readiness (events) until deadline; abort_fd interrupts.
 * Returns WS_OK, WS_TIMEOUT, WS_ABORTED or WS_ERR. */
static int wait_fd(int fd, short events, int abort_fd, int64_t deadline_ms)
{
	for (;;) {
		struct pollfd p[2];
		int n = 1, rc;
		int64_t left = deadline_ms - amdws_now_ms();

		if (left <= 0) return WS_TIMEOUT;
		if (left > 1000) left = 1000;   /* re-check the clock at least once a second */

		p[0].fd = fd; p[0].events = events; p[0].revents = 0;
		if (abort_fd >= 0) {
			p[1].fd = abort_fd; p[1].events = POLLIN; p[1].revents = 0;
			n = 2;
		}
		rc = poll(p, n, (int) left);
		if (rc < 0) {
			if (errno == EINTR) continue;
			return WS_ERR;
		}
		if (n == 2 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) return WS_ABORTED;
		if (rc > 0 && p[0].revents) {
			return WS_OK;   /* includes POLLERR/POLLHUP: the next I/O call reports it */
		}
	}
}

/* ------------------------------------------------------------------------ */
/* raw I/O (plain or TLS)                                                    */

/* Returns bytes read (>0), 0 = would block, WS_CLOSED on EOF, WS_ERR. */
static int io_read(amdws_client_t *ws, void *buf, size_t len)
{
	if (ws->ssl) {
		int r;
		ERR_clear_error();
		r = SSL_read(ws->ssl, buf, (int) len);
		if (r > 0) return r;
		switch (SSL_get_error(ws->ssl, r)) {
		case SSL_ERROR_WANT_READ:
		case SSL_ERROR_WANT_WRITE:
			return 0;
		case SSL_ERROR_ZERO_RETURN:
			return WS_CLOSED;
		case SSL_ERROR_SYSCALL:
			if (r == 0 || errno == 0) return WS_CLOSED;
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
			return WS_ERR;
		default:
			return WS_ERR;
		}
	} else {
		ssize_t r = recv(ws->fd, buf, len, 0);
		if (r > 0) return (int) r;
		if (r == 0) return WS_CLOSED;
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
		return WS_ERR;
	}
}

/* Returns bytes written (>=0; 0 = would block) or WS_ERR/WS_CLOSED.
 * *want receives POLLIN/POLLOUT for the retry (TLS renegotiation). */
static int io_write(amdws_client_t *ws, const void *buf, size_t len, short *want)
{
	*want = POLLOUT;
	if (ws->ssl) {
		int r;
		ERR_clear_error();
		r = SSL_write(ws->ssl, buf, (int) len);
		if (r > 0) return r;
		switch (SSL_get_error(ws->ssl, r)) {
		case SSL_ERROR_WANT_WRITE: return 0;
		case SSL_ERROR_WANT_READ: *want = POLLIN; return 0;
		case SSL_ERROR_ZERO_RETURN: return WS_CLOSED;
		case SSL_ERROR_SYSCALL:
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
			return (errno == EPIPE || errno == ECONNRESET) ? WS_CLOSED : WS_ERR;
		default: return WS_ERR;
		}
	} else {
		ssize_t r = send(ws->fd, buf, len, MSG_NOSIGNAL);
		if (r >= 0) return (int) r;
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
		return (errno == EPIPE || errno == ECONNRESET) ? WS_CLOSED : WS_ERR;
	}
}

static int write_all(amdws_client_t *ws, const void *buf, size_t len, int64_t deadline_ms)
{
	const unsigned char *p = buf;
	while (len) {
		short want;
		int r = io_write(ws, p, len, &want);
		if (r < 0) return r;
		if (r == 0) {
			int w = wait_fd(ws->fd, want, ws->abort_fd, deadline_ms);
			if (w != WS_OK) return w;
			continue;
		}
		p += r;
		len -= (size_t) r;
	}
	return WS_OK;
}

static int rx_reserve(amdws_client_t *ws, size_t extra)
{
	if (ws->rx_len + extra <= ws->rx_cap) return 0;
	{
		size_t cap = ws->rx_cap ? ws->rx_cap : WS_RX_CHUNK;
		unsigned char *n;
		while (cap < ws->rx_len + extra) cap *= 2;
		if (cap > (size_t) WS_MAX_FRAME + 64) return -1;
		n = realloc(ws->rx, cap);
		if (!n) return -1;
		ws->rx = n;
		ws->rx_cap = cap;
	}
	return 0;
}

/* Read everything currently available into rx.  Returns WS_OK, WS_CLOSED, WS_ERR. */
static int rx_fill(amdws_client_t *ws)
{
	int loops = 0;
	for (;;) {
		int r;
		if (rx_reserve(ws, WS_RX_CHUNK) < 0) return WS_ERR;
		r = io_read(ws, ws->rx + ws->rx_len, WS_RX_CHUNK);
		if (r < 0) return r;
		if (r == 0) return WS_OK;
		ws->rx_len += (size_t) r;
		if (++loops > 64) return WS_OK;   /* bounded; caller comes back */
	}
}

/* ------------------------------------------------------------------------ */
/* connect                                                                    */

static int tcp_connect(const amdws_connect_opts_t *o, int64_t deadline_ms, char *err, size_t errlen)
{
	struct addrinfo hints, *res = NULL, *ai;
	char portstr[16];
	int rc, fd = -1, last = WS_ERR;
	char lasterr[128] = "no address";

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(portstr, sizeof(portstr), "%d", o->port);

	/* getaddrinfo() itself cannot be bounded; it runs on the engine's worker
	 * thread, and the channel side enforces the connect deadline on its own. */
	rc = getaddrinfo(o->host, portstr, &hints, &res);
	if (rc != 0) {
		set_err(err, errlen, "resolve %s: %s", o->host, gai_strerror(rc));
		return WS_ERR;
	}
	if (amdws_now_ms() >= deadline_ms) {
		freeaddrinfo(res);
		set_err(err, errlen, "resolve %s: took longer than the connect timeout%s", o->host, "");
		return WS_TIMEOUT;
	}

	for (ai = res; ai; ai = ai->ai_next) {
		int one = 1, soerr = 0, w;
		socklen_t sl = sizeof(soerr);

		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
		if (fd < 0) { snprintf(lasterr, sizeof(lasterr), "socket: %s", strerror(errno)); continue; }
		set_nonblock(fd);
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
		if (errno != EINPROGRESS) {
			snprintf(lasterr, sizeof(lasterr), "connect: %s", strerror(errno));
			close(fd); fd = -1; continue;
		}
		w = wait_fd(fd, POLLOUT, o->abort_fd, deadline_ms);
		if (w != WS_OK) {
			last = w;
			snprintf(lasterr, sizeof(lasterr), "connect: %s", amdws_strerror(w));
			close(fd); fd = -1;
			if (w == WS_ABORTED || w == WS_TIMEOUT) break;
			continue;
		}
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr) {
			snprintf(lasterr, sizeof(lasterr), "connect: %s", strerror(soerr ? soerr : errno));
			close(fd); fd = -1; continue;
		}
		break;
	}
	freeaddrinfo(res);
	if (fd < 0) {
		set_err(err, errlen, "%s%s", lasterr, "");
		return last == WS_OK ? WS_ERR : last;
	}
	return fd;
}

static const char *default_cafile(void)
{
	static const char *c[] = {
		"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
		"/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem", NULL
	};
	int i;
	for (i = 0; c[i]; i++) {
		if (access(c[i], R_OK) == 0) return c[i];
	}
	return NULL;
}

static int tls_setup(amdws_client_t *ws, const amdws_connect_opts_t *o, int64_t deadline_ms, char *err, size_t errlen)
{
#if OPENSSL_VERSION_NUMBER < 0x10100000L
	ws->ctx = SSL_CTX_new(SSLv23_client_method());
#else
	ws->ctx = SSL_CTX_new(TLS_client_method());
#endif
	if (!ws->ctx) { set_err(err, errlen, "TLS: cannot create context%s%s", "", ""); return WS_ERR; }
	SSL_CTX_set_options(ws->ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
	SSL_CTX_set_mode(ws->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

	if (o->tls_verify) {
		const char *ca = (o->tls_cafile && *o->tls_cafile) ? o->tls_cafile : default_cafile();
		int ok = ca ? SSL_CTX_load_verify_locations(ws->ctx, ca, NULL)
		            : SSL_CTX_load_verify_locations(ws->ctx, NULL, "/etc/ssl/certs");
		if (!ok) {
			set_err(err, errlen, "TLS: cannot load CA bundle %s%s", ca ? ca : "/etc/ssl/certs", "");
			return WS_ERR;
		}
		SSL_CTX_set_verify(ws->ctx, SSL_VERIFY_PEER, NULL);
	} else {
		SSL_CTX_set_verify(ws->ctx, SSL_VERIFY_NONE, NULL);
	}

	ws->ssl = SSL_new(ws->ctx);
	if (!ws->ssl) { set_err(err, errlen, "TLS: cannot create session%s%s", "", ""); return WS_ERR; }
	SSL_set_fd(ws->ssl, ws->fd);
	SSL_set_tlsext_host_name(ws->ssl, o->host);          /* SNI */
	if (o->tls_verify && o->tls_check_hostname) {
		X509_VERIFY_PARAM *vp = SSL_get0_param(ws->ssl);
		X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
		if (!X509_VERIFY_PARAM_set1_host(vp, o->host, 0)) {
			set_err(err, errlen, "TLS: cannot set hostname check for %s%s", o->host, "");
			return WS_ERR;
		}
	}

	for (;;) {
		int r, e, w;
		ERR_clear_error();
		r = SSL_connect(ws->ssl);
		if (r == 1) break;
		e = SSL_get_error(ws->ssl, r);
		if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
			w = wait_fd(ws->fd, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, ws->abort_fd, deadline_ms);
			if (w != WS_OK) {
				set_err(err, errlen, "TLS handshake: %s%s", amdws_strerror(w), "");
				return w;
			}
			continue;
		}
		{
			char buf[160];
			long vr = SSL_get_verify_result(ws->ssl);
			unsigned long ee = ERR_peek_last_error();
			if (vr != X509_V_OK) {
				snprintf(buf, sizeof(buf), "certificate: %s", X509_verify_cert_error_string(vr));
			} else if (ee) {
				ERR_error_string_n(ee, buf, sizeof(buf));
			} else {
				snprintf(buf, sizeof(buf), "connection closed");
			}
			set_err(err, errlen, "TLS handshake failed: %s%s", buf, "");
		}
		return WS_ERR;
	}
	return WS_OK;
}

static void b64(const unsigned char *in, int inlen, char *out, size_t outlen)
{
	unsigned char tmp[64];
	int n;
	if ((size_t) (4 * ((inlen + 2) / 3)) + 1 > outlen || inlen > 45) { out[0] = 0; return; }
	n = EVP_EncodeBlock(tmp, in, inlen);
	memcpy(out, tmp, (size_t) n);
	out[n] = 0;
}

/* Case-insensitive header lookup in a raw HTTP response; copies the trimmed value. */
static int http_header(const char *resp, const char *name, char *val, size_t vlen)
{
	size_t nl = strlen(name);
	const char *p = strstr(resp, "\r\n");
	while (p && p[2] && !(p[2] == '\r' && p[3] == '\n')) {
		const char *line = p + 2, *eol = strstr(line, "\r\n");
		if (!eol) break;
		if ((size_t) (eol - line) > nl && strncasecmp(line, name, nl) == 0 && line[nl] == ':') {
			const char *v = line + nl + 1;
			size_t l;
			while (*v == ' ' || *v == '\t') v++;
			l = (size_t) (eol - v);
			while (l && (v[l - 1] == ' ' || v[l - 1] == '\t')) l--;
			if (l >= vlen) l = vlen - 1;
			memcpy(val, v, l);
			val[l] = 0;
			return 1;
		}
		p = eol;
	}
	return 0;
}

static int http_upgrade(amdws_client_t *ws, const amdws_connect_opts_t *o, int64_t deadline_ms, char *err, size_t errlen)
{
	unsigned char keyraw[16], sha[SHA_DIGEST_LENGTH];
	char key[32], expect[40], accept_hdr[64], req[1024], hosthdr[300], resp[WS_MAX_HANDSHAKE + 1];
	char tmp[128];
	size_t rlen = 0;
	int n, rc;
	const char *hdr_end = NULL;

	if (RAND_bytes(keyraw, sizeof(keyraw)) != 1) {
		set_err(err, errlen, "handshake: no randomness%s%s", "", "");
		return WS_ERR;
	}
	b64(keyraw, sizeof(keyraw), key, sizeof(key));
	snprintf(tmp, sizeof(tmp), "%s%s", key, WS_GUID);
	SHA1((const unsigned char *) tmp, strlen(tmp), sha);
	b64(sha, SHA_DIGEST_LENGTH, expect, sizeof(expect));

	if (strchr(o->host, ':')) {
		snprintf(hosthdr, sizeof(hosthdr), "[%s]:%d", o->host, o->port);
	} else {
		snprintf(hosthdr, sizeof(hosthdr), "%s:%d", o->host, o->port);
	}
	n = snprintf(req, sizeof(req),
				 "GET %s HTTP/1.1\r\n"
				 "Host: %s\r\n"
				 "Upgrade: websocket\r\n"
				 "Connection: Upgrade\r\n"
				 "Sec-WebSocket-Key: %s\r\n"
				 "Sec-WebSocket-Version: 13\r\n"
				 "User-Agent: %s\r\n"
				 "\r\n",
				 (o->path && *o->path) ? o->path : "/", hosthdr, key,
				 (o->user_agent && *o->user_agent) ? o->user_agent : "mod_amd_ws");
	if (n <= 0 || (size_t) n >= sizeof(req)) {
		set_err(err, errlen, "handshake: request too long%s%s", "", "");
		return WS_ERR;
	}
	rc = write_all(ws, req, (size_t) n, deadline_ms);
	if (rc != WS_OK) {
		set_err(err, errlen, "handshake send: %s%s", amdws_strerror(rc), "");
		return rc;
	}

	/* Read until the end of the headers.  Bytes after it are the first frames. */
	while (!hdr_end) {
		int r;
		if (rlen >= WS_MAX_HANDSHAKE) {
			set_err(err, errlen, "handshake: response headers too large%s%s", "", "");
			return WS_ERR;
		}
		r = io_read(ws, resp + rlen, WS_MAX_HANDSHAKE - rlen);
		if (r == 0) {
			int w = wait_fd(ws->fd, POLLIN, ws->abort_fd, deadline_ms);
			if (w != WS_OK) {
				set_err(err, errlen, "handshake: no response (%s)%s", amdws_strerror(w), "");
				return w;
			}
			continue;
		}
		if (r < 0) {
			set_err(err, errlen, "handshake: connection %s before the response%s",
					r == WS_CLOSED ? "closed" : "failed", "");
			return WS_ERR;
		}
		rlen += (size_t) r;
		resp[rlen] = 0;
		hdr_end = strstr(resp, "\r\n\r\n");
	}

	if (strncmp(resp, "HTTP/1.1 101", 12) != 0 && strncmp(resp, "HTTP/1.0 101", 12) != 0) {
		char line[80];
		const char *e = strstr(resp, "\r\n");
		size_t l = e ? (size_t) (e - resp) : strlen(resp);
		if (l >= sizeof(line)) l = sizeof(line) - 1;
		memcpy(line, resp, l);
		line[l] = 0;
		set_err(err, errlen, "handshake rejected: %s%s", line, "");
		return WS_ERR;
	}
	if (!http_header(resp, "Sec-WebSocket-Accept", accept_hdr, sizeof(accept_hdr)) || strcmp(accept_hdr, expect) != 0) {
		set_err(err, errlen, "handshake: bad or missing Sec-WebSocket-Accept%s%s", "", "");
		return WS_ERR;
	}

	/* keep frame bytes that arrived with the headers */
	{
		size_t used = (size_t) (hdr_end + 4 - resp);
		if (rlen > used) {
			if (rx_reserve(ws, rlen - used) < 0) return WS_ERR;
			memcpy(ws->rx, resp + used, rlen - used);
			ws->rx_len = rlen - used;
		}
	}
	return WS_OK;
}

static void amdws_free(amdws_client_t *ws)
{
	if (!ws) return;
	if (ws->ssl) SSL_free(ws->ssl);
	if (ws->ctx) SSL_CTX_free(ws->ctx);
	if (ws->fd >= 0) close(ws->fd);
	free(ws->rx);
	free(ws->msg);
	free(ws->out);
	free(ws);
}

int amdws_connect(amdws_client_t **out, const amdws_connect_opts_t *o, int64_t deadline_ms, char *err, size_t errlen)
{
	amdws_client_t *ws;
	int rc;

	*out = NULL;
	if (err && errlen) err[0] = 0;
	amdws_global_init();

	if (!o || !o->host || !*o->host || o->port <= 0 || o->port > 65535) {
		set_err(err, errlen, "invalid host or port%s%s", "", "");
		return WS_ERR;
	}
	ws = calloc(1, sizeof(*ws));
	if (!ws) return WS_ERR;
	ws->fd = -1;
	ws->abort_fd = o->abort_fd;

	rc = tcp_connect(o, deadline_ms, err, errlen);
	if (rc < 0) { amdws_free(ws); return rc; }
	ws->fd = rc;

	if (o->tls && (rc = tls_setup(ws, o, deadline_ms, err, errlen)) != WS_OK) { amdws_free(ws); return rc; }
	if ((rc = http_upgrade(ws, o, deadline_ms, err, errlen)) != WS_OK) { amdws_free(ws); return rc; }

	*out = ws;
	return WS_OK;
}

int amdws_fd(const amdws_client_t *ws)
{
	return ws ? ws->fd : -1;
}

int amdws_peer_close_code(const amdws_client_t *ws)
{
	return ws ? ws->peer_close_code : 0;
}

/* ------------------------------------------------------------------------ */
/* frames                                                                     */

int amdws_send(amdws_client_t *ws, int opcode, const void *data, size_t len, int64_t deadline_ms)
{
	unsigned char hdr[14], mask[4];
	unsigned char stackbuf[1024];
	unsigned char *buf;
	size_t hl = 0, i;
	int rc;

	if (!ws || ws->closed) return WS_CLOSED;
	if (RAND_bytes(mask, 4) != 1) return WS_ERR;

	hdr[hl++] = (unsigned char) (0x80 | (opcode & 0x0F));
	if (len < 126) {
		hdr[hl++] = (unsigned char) (0x80 | len);
	} else if (len <= 0xFFFF) {
		hdr[hl++] = 0x80 | 126;
		hdr[hl++] = (unsigned char) (len >> 8);
		hdr[hl++] = (unsigned char) (len & 0xFF);
	} else {
		int k;
		hdr[hl++] = 0x80 | 127;
		for (k = 7; k >= 0; k--) hdr[hl++] = (unsigned char) (((uint64_t) len >> (8 * k)) & 0xFF);
	}
	memcpy(hdr + hl, mask, 4);
	hl += 4;

	/* one write per frame: header + masked payload in one buffer */
	if (hl + len <= sizeof(stackbuf)) {
		buf = stackbuf;
	} else if (!(buf = malloc(hl + len))) {
		return WS_ERR;
	}
	memcpy(buf, hdr, hl);
	for (i = 0; i < len; i++) buf[hl + i] = ((const unsigned char *) data)[i] ^ mask[i & 3];

	rc = write_all(ws, buf, hl + len, deadline_ms);
	if (buf != stackbuf) free(buf);
	if (rc == WS_CLOSED || rc == WS_ERR) ws->closed = 1;
	if (rc == WS_OK && opcode == WS_OP_CLOSE) ws->close_sent = 1;
	return rc;
}

int amdws_pending(const amdws_client_t *ws)
{
	if (!ws) return 0;
	if (ws->ssl && SSL_pending(ws->ssl) > 0) return 1;
	if (ws->rx_len >= 2) {
		/* is a complete frame buffered? */
		const unsigned char *p = ws->rx;
		uint64_t plen = p[1] & 0x7F;
		size_t need = 2;
		if (plen == 126) { if (ws->rx_len < 4) return 0; plen = ((uint64_t) p[2] << 8) | p[3]; need = 4; }
		else if (plen == 127) {
			int k;
			if (ws->rx_len < 10) return 0;
			plen = 0;
			for (k = 0; k < 8; k++) plen = (plen << 8) | p[2 + k];
			need = 10;
		}
		if (p[1] & 0x80) need += 4;
		return ws->rx_len >= need + plen;
	}
	return 0;
}

static int msg_append(amdws_client_t *ws, const unsigned char *d, size_t l)
{
	if (ws->msg_len + l + 1 > ws->msg_cap) {
		size_t cap = ws->msg_cap ? ws->msg_cap : 1024;
		char *n;
		while (cap < ws->msg_len + l + 1) cap *= 2;
		if (cap > (size_t) WS_MAX_FRAME) return -1;
		if (!(n = realloc(ws->msg, cap))) return -1;
		ws->msg = n;
		ws->msg_cap = cap;
	}
	memcpy(ws->msg + ws->msg_len, d, l);
	ws->msg_len += l;
	ws->msg[ws->msg_len] = 0;
	return 0;
}

/* Parse one frame from rx.  Returns 1 = message complete in ws->msg,
 * 0 = need more bytes / control frame consumed (call again), <0 error/closed.
 * *consumed is set when a frame was taken off the buffer. */
static int parse_one(amdws_client_t *ws, int *consumed)
{
	unsigned char *p = ws->rx;
	uint64_t plen;
	size_t hl = 2;
	int fin, op, masked;
	unsigned char mk[4] = { 0, 0, 0, 0 };
	unsigned char *payload;

	*consumed = 0;
	if (ws->rx_len < 2) return 0;
	fin = (p[0] & 0x80) != 0;
	op = p[0] & 0x0F;
	masked = (p[1] & 0x80) != 0;
	plen = p[1] & 0x7F;
	if (p[0] & 0x70) return WS_ERR;                 /* no extensions negotiated */
	if (plen == 126) {
		if (ws->rx_len < 4) return 0;
		plen = ((uint64_t) p[2] << 8) | p[3];
		hl = 4;
	} else if (plen == 127) {
		int k;
		if (ws->rx_len < 10) return 0;
		plen = 0;
		for (k = 0; k < 8; k++) plen = (plen << 8) | p[2 + k];
		hl = 10;
	}
	if (plen > WS_MAX_FRAME) return WS_ERR;
	if (masked) {
		if (ws->rx_len < hl + 4) return 0;
		memcpy(mk, p + hl, 4);
		hl += 4;
	}
	if (ws->rx_len < hl + plen) return 0;

	payload = p + hl;
	if (masked) {
		uint64_t i;
		for (i = 0; i < plen; i++) payload[i] ^= mk[i & 3];
	}

	*consumed = 1;
	{
		int ret = 0;
		switch (op) {
		case WS_OP_PING:
			if (!fin || plen > 125) { ret = WS_ERR; break; }
			amdws_send(ws, WS_OP_PONG, payload, (size_t) plen, amdws_now_ms() + 1000);
			break;
		case WS_OP_PONG:
			break;
		case WS_OP_CLOSE:
			ws->peer_close_code = plen >= 2 ? ((payload[0] << 8) | payload[1]) : 1005;
			if (!ws->close_sent) {
				unsigned char code[2] = { 0x03, 0xE8 };   /* 1000 */
				amdws_send(ws, WS_OP_CLOSE, code, 2, amdws_now_ms() + WS_CLOSE_WAIT_MS);
			}
			ws->closed = 1;
			ret = WS_CLOSED;
			break;
		case WS_OP_TEXT:
		case WS_OP_BINARY:
			if (ws->msg_opcode) { ret = WS_ERR; break; }   /* new message inside a fragmented one */
			ws->msg_len = 0;
			if (msg_append(ws, payload, (size_t) plen) < 0) { ret = WS_ERR; break; }
			if (fin) { ret = 1; ws->msg_opcode = op; }
			else ws->msg_opcode = op | 0x100;            /* assembling */
			break;
		case WS_OP_CONT:
			if (!(ws->msg_opcode & 0x100)) { ret = WS_ERR; break; }
			if (msg_append(ws, payload, (size_t) plen) < 0) { ret = WS_ERR; break; }
			if (fin) { ws->msg_opcode &= 0xFF; ret = 1; }
			break;
		default:
			ret = WS_ERR;
		}
		/* drop the frame from rx */
		{
			size_t fl = hl + (size_t) plen;
			memmove(ws->rx, ws->rx + fl, ws->rx_len - fl);
			ws->rx_len -= fl;
		}
		return ret;
	}
}

int amdws_read(amdws_client_t *ws, amdws_msg_t *msg)
{
	int rc, consumed, fill_rc = WS_OK, filled = 0;

	if (!ws) return WS_ERR;
	if (ws->closed && !ws->rx_len) return WS_CLOSED;

	for (;;) {
		rc = parse_one(ws, &consumed);
		if (rc == 1) {
			int op = ws->msg_opcode & 0xFF;
			/* hand the message over; keep ws->msg for the next one */
			if (ws->out_cap < ws->msg_len + 1) {
				char *n = realloc(ws->out, ws->msg_len + 1);
				if (!n) return WS_ERR;
				ws->out = n;
				ws->out_cap = ws->msg_len + 1;
			}
			memcpy(ws->out, ws->msg, ws->msg_len + 1);
			msg->opcode = op;
			msg->data = ws->out;
			msg->len = ws->msg_len;
			ws->msg_opcode = 0;
			ws->msg_len = 0;
			return 1;
		}
		if (rc < 0) {
			ws->closed = 1;
			return rc;
		}
		if (consumed) continue;           /* control frame handled, look again */
		if (filled) break;                /* already read the socket this call */
		fill_rc = rx_fill(ws);
		filled = 1;
		if (fill_rc != WS_OK) {
			/* parse what we have first; report EOF afterwards */
			rc = parse_one(ws, &consumed);
			if (rc == 1) { filled = 0; ws->closed = 1; continue; }
			ws->closed = 1;
			return fill_rc == WS_CLOSED ? WS_CLOSED : WS_ERR;
		}
	}
	return 0;
}

void amdws_close(amdws_client_t *ws, int code)
{
	if (!ws) return;
	if (!ws->closed && !ws->close_sent && ws->fd >= 0) {
		unsigned char c[2];
		int saved = ws->abort_fd;
		c[0] = (unsigned char) (code >> 8);
		c[1] = (unsigned char) (code & 0xFF);
		ws->abort_fd = -1;       /* the close must go out even when aborting */
		amdws_send(ws, WS_OP_CLOSE, c, 2, amdws_now_ms() + WS_CLOSE_WAIT_MS);
		ws->abort_fd = saved;
	}
	if (ws->ssl && !ws->closed) {
		SSL_shutdown(ws->ssl);   /* non-blocking, one attempt */
	}
	amdws_free(ws);
}
