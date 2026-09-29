# Architecture

```text
 channel thread (or whoever reads the channel)          worker thread (one per detection)
 ------------------------------------------------       ------------------------------------
 media bug READ callback, every frame                   DNS, TCP, TLS, HTTP upgrade (deadline)
   switch_core_media_bug_read()  (one per callback)     config TEXT frame
   0xFFFF fill? -> "no audio"                            loop:
   resample to 8 kHz if needed                             schedule marks -> BINARY frames
   amd_engine_feed()  --- mutex, audio buffer --->         poll(socket, wake pipe, <=100 ms)
   amd_engine_check() <-- mutex, result ---------          replies -> classifier -> result
 application loop: read_frame / playback                   EOF finalisation, detection window
   verdict -> CF_BREAK stops the prompt                  exit: {"eof":1}, CLOSE 1000, free
 set variables, fire amd_ws::result, api_on_*
```

## Files

| File | Role |
|---|---|
| `src/mod_amd_ws.c` | FreeSWITCH glue. Loads `amd_ws.conf.xml`, parses arguments, options and channel variables, builds the config frame. Owns the media bug, sets variables, fires the event. Provides the `amd_ws` application and the `uuid_amd_ws` / `amd_ws` APIs. |
| `src/amd_engine.c` | One detection. Holds the audio buffer, runs the worker thread, and implements the schedule, the `amd.py` classifier, the EOF finalisation, every timeout, the local fallback and the counters. Plain C + pthreads, no FreeSWITCH types. |
| `src/ws_client.c` | RFC 6455 client on non-blocking sockets and OpenSSL. Every wait is a `poll()` bounded by a deadline and interruptible through the engine's wake pipe. |
| `src/local_amd.c` | Energy/word-count detector (Asterisk `app_amd`, as in `mod_amd`, with its bugs fixed). |

## Rules the code keeps

- **The channel never waits on the network.** The channel side only locks
  the engine mutex to append audio or read the result. Connect, send and
  receive happen on the worker.
- **Every wait is bounded.** Socket waits use deadlines. `getaddrinfo()`
  cannot be bounded, so the channel side runs a watchdog: past
  `connect_timeout_ms` + 500 ms without a connection, the call ends
  `CONNECTION_ERROR`, and the worker exits whenever the lookup returns.
- **First result wins.** The service verdict, a timeout, an error, a hangup
  and the local fallback all go through one function under the mutex.
- **Lifetimes.** The engine is reference counted: the channel holds one
  reference and the worker the other. The channel releases its reference
  without waiting, so a hangup never waits for the socket to close. The
  worker frees the engine when it exits.
- **Module unload is safe.** Each detection holds a read lock on the module
  interface, so `unload mod_amd_ws` answers "in use" while calls are
  detecting. Shutdown waits up to 10 s for worker threads. If one is still
  inside `getaddrinfo()`, the module reports `NOUNLOAD` and stays mapped
  instead of crashing.
- **No symbol leaks.** A linker version script exports only
  `mod_amd_ws_module_interface`, and internal names are prefixed with
  `amdws_`. An unprefixed `ws_close()` once bound to sofia-sip's `ws_close()`
  and crashed the switch; the version script prevents that whole class of bug.

## Media bug details

- Flags: `SMBF_READ_STREAM | SMBF_NO_PAUSE | SMBF_ONE_ONLY`. The bug is
  read-only, so the call's media is unchanged. `ONE_ONLY` refuses a second
  detection on the same channel.
- Exactly one `switch_core_media_bug_read()` per `SWITCH_ABC_TYPE_READ`
  callback. On an empty buffer the core does not fail; it returns a synthetic
  frame filled with `0xFF`. Draining in a loop would feed the detectors
  invented silence.
- Missing media (no RTP) reaches the bug as frames where every sample is
  `0xFFFF`, the core's comfort-noise fill. The engine treats those as "no
  audio": they are not sent and they count towards the EOF streak. The local
  detector sees them as silence, as `mod_amd` would.

## Foreground vs background

| | `amd_ws` application | `uuid_amd_ws <uuid> start` |
|---|---|---|
| Who reads the channel | the application loop (or its playback) | whatever the channel is doing (bridge, IVR, park) |
| Who applies the result | the application, after removing the bug | the bug callback on the frame after the verdict; it returns `SWITCH_FALSE` so the core removes the bug |
| Hangup / stop | loop ends → `HANGUP` | bug `CLOSE` → `HANGUP`, or `STOPPED` for `uuid_amd_ws stop` |
| Extra | `playfile` in parallel, `CF_BREAK` stops it at the verdict | `api_on_amd_ws_result*` commands run |
