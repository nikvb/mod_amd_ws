# Wire protocol

`amd_ws` speaks the protocol of the amdy.io EAGI client `amd.py` (July 2026),
byte for byte the same as `app_amd_ws` for Asterisk. There is one WebSocket
connection per detection. TEXT frames carry JSON or plain text, BINARY frames
carry audio.

## Sequence

```text
 FreeSWITCH (amd_ws worker)                                AMD service
 GET / HTTP/1.1  Upgrade: websocket                        --->
                                                           <---  101 Switching Protocols
 TEXT   {"config":{"sample_rate":8000,"VID":"...",
                   "phone":"...","country_code":"...",
                   "caller_id":"..."}}                     --->
 (t = 0: first audio frame captured from the channel)
 BINARY audio 0-500 ms          at t = 500                 --->
                                                           <---  TEXT ack
 BINARY audio 500-1000 ms       at t = 1000                --->
                                                           <---  TEXT ack
 BINARY audio 1000-1500 ms      at t = 1500                --->
                                                           <---  TEXT "HUMAN"
 TEXT   {"eof":1}                                          --->
 CLOSE  1000                                               --->
```

## Connection

- URL `ws://host:port/path` (path `/` by default), or `wss://` with `tls=true` or option `s`.
- The HTTP upgrade is RFC 6455 with a random `Sec-WebSocket-Key`. The client
  checks `Sec-WebSocket-Accept`. It requests no subprotocol and no extension.
- TLS sends SNI and verifies the chain against `tls_cafile` or the system
  bundle, and with `tls_check_hostname` also checks the name.
- DNS, TCP, TLS and the upgrade together are bounded by `connect_timeout_ms`.
  The call itself also enforces the limit, so a DNS lookup that hangs cannot
  hold a call. Failure → `HUMAN`/`CONNECTION_ERROR`.
- Audio captured during the connect is kept and sent at the next mark.

## Config frame

Keys in this order, optional keys left out rather than sent empty:

```json
{"config":{"sample_rate":8000,"VID":"V9211234560000123","phone":"3125551212","country_code":"1","caller_id":"3125550100"}}
```

| Key | Source |
|---|---|
| `sample_rate` | always `8000` (audio is resampled to 8 kHz when the channel is wideband) |
| `VID` | argument 3, `${amd_ws_vid}`, `${caller_id_name}`, else `Unknown` |
| `phone` | option `p()`, `${amd_ws_phone}` |
| `country_code` | option `k()`, `${amd_ws_country_code}` |
| `caller_id` | option `i()`, `${amd_ws_caller_id}`, else (with `send_caller_id=true`) `effective_caller_id_number` / `origination_caller_id_number` / `caller_id_number` |
| extra members | `extra_config` JSON object, appended after `caller_id` |

Strings are JSON-escaped. Valid UTF-8 passes through; invalid bytes become `?`.

## Audio

- Signed 16-bit little-endian linear PCM, 8000 Hz, mono. The module reads the
  callee's audio through a read-only media bug; wideband channels are resampled
  and stereo channels use the first channel.
- At each `send_schedule` mark (500, 1000, 1500, 2000, 3000, ... 9000 ms from the
  first captured frame) everything accumulated since the previous send goes out
  as one BINARY frame. After the last mark a frame goes out whenever
  `chunk_bytes` (8000) have accumulated or `fallback_interval_ms` (1000) have
  passed with audio pending.
- Nothing is dropped or truncated. A backlog above 160 000 bytes (10 s), which
  only happens after a pathological connect, is split into several frames.
- Frames the core fills because no RTP arrived are **not** audio. They are not
  sent and they do not reset the no-audio streak. The core marks them by
  filling every sample with `0xFFFF`. Digital silence is audio.

## Replies

Every TEXT frame is classified with `amd.py`'s rule, case-sensitive substring:

1. contains `HUMAN` → `HUMAN`/`HUMAN`;
2. else contains `AMD` (not followed by `Y`) or `MACHINE` → `MACHINE`/`<the text>`;
3. else an acknowledgement; detection continues.

Fragmented messages are reassembled. PING is answered with PONG. A CLOSE or a
socket error before a verdict → `HUMAN`/`PROCESSING_ERROR`. The reply is read
the moment it arrives, not only after a send.

## EOF finalisation

A schedule mark at which nothing was captured since the previous send counts
towards a streak. Any captured frame resets it. When the streak reaches
`eof_no_audio_streak` (2) after at least one chunk was sent, the module sends
`{"eof":1}` and waits `eof_wait_ms` (3000) for one reply:

| Reply | Result |
|---|---|
| contains `HUMAN` | `HUMAN`/`HUMAN` |
| contains `AMD`/`MACHINE` | `MACHINE`/`<text>` |
| anything else | `NOTSURE`/`EOF_INCONCLUSIVE` |
| none in time / error | `NOTSURE`/`EOF_ERROR` |

## End of detection

On every exit, if connected: TEXT `{"eof":1}`, then CLOSE with code 1000, then
the socket is closed. This happens on the worker thread and takes at most about
0.5 s. The channel does not wait for it.

## Timeouts

| Phase | Limit | Result |
|---|---|---|
| Connect | `connect_timeout_ms` from the start (never later than the detection window) | `HUMAN`/`CONNECTION_ERROR` |
| Detection | `timeout_ms` from the first captured frame (from the start while none) | `NOTSURE`/`SERVER_TIMEOUT`, or `NOTSURE`/`NOAUDIODATA-<timeout_ms>` if no audio was ever captured |
| Grace | `result_grace_ms` after the window (default 0) | as above |
| EOF | `eof_wait_ms` after `{"eof":1}` | `NOTSURE`/`EOF_ERROR` |
| Hangup | immediately | `HANGUP`/`HANGUP` |

`AMDSTATS` = `<elapsed_ms>-<audio_ms_sent>-<chunks_sent>-<bytes_sent>`, where
`audio_ms_sent` is bytes / 16.
