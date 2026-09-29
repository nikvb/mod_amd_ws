# Tests

| | Needs | Runs |
|---|---|---|
| `make test-unit` (`test/unit.sh`) | gcc, OpenSSL, Python 3 + `websockets` < 13 | the engine and WebSocket client against the mock server, no FreeSWITCH |
| `test/fs_test.py` | a FreeSWITCH binary and its modules (`mod_loopback`, `mod_sndfile`, `mod_dptools`, `mod_commands`, `mod_event_socket`, `mod_dialplan_xml`, `mod_tone_stream`) | 32 scenarios on real calls |

Both run as a normal user and change nothing on the system.

```bash
make                                              # builds ./mod_amd_ws.so
FS_BIN=/usr/bin/freeswitch python3 test/fs_test.py
python3 test/fs_test.py --only human,eof_human -v --keep
python3 test/fs_test.py --list
```

`fs_test.py` creates a scratch directory holding a generated `freeswitch.xml`,
a module directory of symlinks (the switch's modules plus the freshly built
`mod_amd_ws.so`), the mock AMD server (plain and TLS, with a self-signed
certificate for `127.0.0.1`), a black-hole listener and a dead port. Every
scenario is one loopback call. The far leg plays a generated recording and
then parks (no media), hangs up or plays silence. The near leg runs `amd_ws`,
or `uuid_amd_ws` in background mode. The harness checks the
`CUSTOM amd_ws::result` event and what the mock recorded on the wire:

- config frame keys and order;
- `caller_id`;
- the send schedule;
- `{"eof":1}`;
- CLOSE 1000.

| Group | Scenarios |
|---|---|
| Verdicts | human, machine, amd_detected, json_human, amdy_guard, fragmented, ping |
| EOF finalisation | eof_human, eof_ack, eof_silent |
| Timeouts | server_timeout, noaudio |
| Errors | conn_refused, blackhole (connect timeout), rejected (HTTP 403), server_close |
| Channel | hangup, playback, play_delay, wideband (L16 16 kHz → resampler) |
| Config frame | config_keys (`p()` `k()` `i()`, key order) |
| Local detector | local_human, local_machine, local_silence, compare, compare_wait, fallback |
| Transport | tls (`wss://` with chain and hostname verification) |
| Background | background (+ `api_on_amd_ws_result`), bg_stop |
| Load | concurrency (20 simultaneous calls) |
| Module | lifecycle: `unload` refused during a call, no worker left after it, `reload` |

On a failure the scratch directory is kept; its `freeswitch.log` has every
`AMD_WS:` trace line. `FS_GDB=1` runs FreeSWITCH under gdb and prints all
thread backtraces on a crash.

`gen_audio.py` writes the synthetic recordings, including the benchmark corpus
in `audio/bench/`. `mock_amd_server.py` and `blackhole_server.py` are shared
with `app_amd_ws` for Asterisk. The mock's `/oracle` path is used by the
benchmark.
