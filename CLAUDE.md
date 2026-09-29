# CLAUDE.md

`mod_amd_ws` is a FreeSWITCH module: application `amd_ws`, API `uuid_amd_ws` /
`amd_ws`, event `CUSTOM amd_ws::result`. It streams the callee's first seconds
to the amdy.io AMD service over WebSocket, using the same protocol and
vocabulary as `app_amd_ws` for Asterisk (`/home/na/app_amd_ws`, github
nikvb/amd) and `amd.py` (July 2026). It also has a built-in energy detector
(Asterisk `app_amd` algorithm).

Audience for user-facing text: a FreeSWITCH/ViciDial administrator. Use
precise commands and tables; no marketing.

## Layout
| Path | |
|---|---|
| `src/mod_amd_ws.c` | FreeSWITCH glue (config, app, API, media bug, variables, event) |
| `src/amd_engine.c` | per-call engine + worker thread; no FreeSWITCH types |
| `src/ws_client.c` | RFC 6455 client; every wait bounded |
| `src/local_amd.c` | energy detector |
| `install.sh` | **generated** by `make installer` from `tools/install.sh.in`; never edit; `make check-installer` in CI |
| `test/fs_test.py` | 32 scenarios on a real FreeSWITCH; `test/unit.sh` without FreeSWITCH |
| `bench/bench.py` | comparison in `docs/performance.md` |

## Rules
- The channel thread never waits on the network; every wait has a deadline.
- One `switch_core_media_bug_read()` per READ callback (the core returns a 0xFF frame on an empty buffer).
- Only `mod_amd_ws_module_interface` is exported (`src/exports.map`); prefix internal symbols `amdws_`
  (an unprefixed `ws_close` bound to sofia-sip's and crashed the switch).
- Status/cause words are those of `app_amd_ws`; add new ones only with a CHANGELOG entry.
- Behaviour change: update README, the relevant docs page, CHANGELOG, add a scenario to `test/fs_test.py`, run `make installer`.

## Dev boxes
- Local FreeSWITCH 1.10.12 (no sudo): `. /home/na/fs-dev/env.sh`, `PKG_CONFIG_PATH=/home/na/fs-dev/prefix/lib/pkgconfig make`,
  `FS_BIN=/home/na/fs-dev/prefix/bin/freeswitch python3 test/fs_test.py`.
- api14.amdy.io: Docker; container bridge DNS is broken, so use `--network host`; image `amdws-fs-alpine`
  (FreeSWITCH 1.10.11 apk). api14 runs haproxy on :2700 (the AMD service); never bind SIP or :2700 there.
