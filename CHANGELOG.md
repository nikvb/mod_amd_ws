# Changelog

## 1.0.1 (2026-10-31)

Fixes from a code review. No change to the wire protocol or the result vocabulary.

- A verdict that arrived in the same read as the server's CLOSE/FIN was dropped
  and the call returned `HUMAN`/`PROCESSING_ERROR` (a machine sent to an agent).
  Every complete message in the buffer is now handed over before EOF is
  reported. Unit case `burst_close`.
- `api_on_amd_ws_result*` commands run from the media bug's CLOSE callback, after
  the core released the bug lock. Before, they ran inside the READ callback, and
  a command that adds or removes a media bug (`uuid_record`, `uuid_amd_ws`)
  deadlocked the call. Scenario `bg_record_hook`.
- An invalid `send_schedule` restored the default schedule only in name: it reset
  the whole configuration read so far (host fell back to 127.0.0.1, so every call
  returned `CONNECTION_ERROR`). Now only the schedule falls back to the default.
- Installer: the distribution dev package is pinned to the installed FreeSWITCH
  package version and never upgrades anything; if that version is not available
  the matching source tarball is used, as before.
- Installer: works on bash older than 4.4 (CentOS/RHEL 7), where an empty array
  under `set -u` aborted the run when all build tools were already installed.

## 1.0.0 (2026-09-29)

First release.

- `amd_ws` dialplan application: `amd_ws [host[,port[,vid[,timeout_ms[,playfile[,options]]]]]]`.
  The arguments, options and result vocabulary are those of `AMD_WS()` for Asterisk
  (`app_amd_ws` 2.x) and of the amdy.io EAGI client `amd.py` (July 2026).
- Wire protocol identical to `app_amd_ws`: the config frame (`sample_rate`,
  `VID`, `phone`, `country_code`, `caller_id`, `extra_config`), 11-mark send
  schedule, chunk/fallback sends, EOF finalisation, `{"eof":1}` and CLOSE 1000
  on every exit.
- Variables `AMDSTATUS`, `AMDCAUSE`, `AMDSTATS`, `AMDRESPONSE`, `AMDELAPSED`,
  plus `amd_result` / `amd_cause` for dialplans written for `mod_amd`, and
  `amd_ws_*` details.
- `uuid_amd_ws <uuid> start|stop`: background detection on a media bug while
  the channel bridges or runs an IVR; `api_on_amd_ws_result*` hooks.
- `CUSTOM amd_ws::result` event.
- Built-in energy detector (Asterisk `app_amd` algorithm, `mod_amd` parameter
  names), with modes `local` (`L`), `compare` (`C`), `compare_wait_local`,
  and `local_fallback` (`F`) on connection/processing errors.
- Parallel playback with delay (`playfile`, `d(ms)`); stopped at the verdict.
- Wideband and stereo channels resampled/downmixed to 8 kHz mono; missing
  media (comfort-noise fill) is not sent and drives the EOF finalisation.
- Own RFC 6455 client (ws/wss, SNI, chain and hostname verification). No
  libwebsockets or libks. Every wait is bounded; the channel thread never
  waits on the network.
- `amd_ws status|reload` API with settings and counters.
- One-line installer with embedded sources. It finds headers via pkg-config,
  the distribution dev package or the matching FreeSWITCH source tarball, and
  gates on undefined symbols, exports and OpenSSL.
- Integration tests (32 scenarios on real FreeSWITCH 1.10.11 musl and 1.10.12
  glibc) and a benchmark against native `mod_amd`
  ([docs/performance.md](docs/performance.md)).
- Found while benchmarking: seanbright/mod_amd's frame-energy loop reads every
  second sample and past the end of the frame, which makes its verdicts wrong
  and unrepeatable. The local detector here implements the algorithm without
  that bug.
