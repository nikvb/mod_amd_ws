# Changelog

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
