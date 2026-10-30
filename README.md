# mod_amd_ws

Answering machine detection for FreeSWITCH that streams the first seconds of an
answered call to the [amdy.io](https://amdy.io) AMD service over WebSocket and
sets `AMDSTATUS` / `AMDCAUSE` on the channel.

It speaks the same protocol, send schedule and result vocabulary as
`app_amd_ws` for Asterisk and the amdy.io EAGI client `amd.py`. It also
contains the energy/word-count detector of `mod_amd` (Asterisk `app_amd`
algorithm), so you can run it without the network, compare both detectors on
live traffic, or fall back to it when the service is unreachable.

| | |
|---|---|
| FreeSWITCH | 1.10.x, 1.11.x (tested: 1.10.11 Alpine/musl packages, 1.10.12 source build on Ubuntu 22.04/glibc) |
| Dependencies | OpenSSL (already linked by FreeSWITCH). No libwebsockets, no libks. |
| License | MPL 1.1 (as FreeSWITCH). The local detector derives from seanbright/mod_amd (CC0). |

## Step-by-step install

You need a FreeSWITCH 1.10.x or 1.11.x server, a root shell on it, and an
amdy.io account (https://app.amdy.io).

**1. Register the server's public IP with amdy.io.** The service accepts
calls by source IP; there is no API key in the module. Find the IP, then sign
in to https://app.amdy.io, open **IP management** and add it.

```bash
curl -s https://api.ipify.org; echo
```

**2. Check that the server can reach the service** (outbound TCP 2700):

```bash
timeout 5 bash -c '</dev/tcp/api.amdy.io/2700' && echo "port 2700 open"
```

**3. Install the module** (builds from this repository's source and loads it):

```bash
curl -fsSL https://raw.githubusercontent.com/nikvb/mod_amd_ws/main/install.sh | bash -s -- -y
```

**4. Check that it loaded:**

```bash
fs_cli -x 'module_exists mod_amd_ws'      # prints: true
fs_cli -x 'amd_ws status'                 # endpoint api.amdy.io:2700, counters
```

**5. Add detection to your outbound dialplan**, right after the call is
answered (full example in [Dialplan](#dialplan)):

```xml
<action application="amd_ws" data=""/>
<action application="log" data="INFO AMD ${AMDSTATUS}/${AMDCAUSE}"/>
<action application="execute_extension" data="amd_${AMDSTATUS} XML ${context}"/>
```

**6. Reload the dialplan:**

```bash
fs_cli -x 'reloadxml'
```

**7. Place a test call** to a phone you can answer, and read the result:

```bash
grep 'AMD_WS:' /var/log/freeswitch/freeswitch.log | tail -5
```

Answer and say "hello": expect `HUMAN`. Let it go to voicemail: expect
`MACHINE`. Detections also appear in your amdy.io dashboard.

**8. Route on the result.** `AMDSTATUS` is `HUMAN`, `MACHINE`, `NOTSURE` or
`HANGUP`; `AMDCAUSE` says why ([Status and cause](#status-and-cause)). Create
the `amd_HUMAN`, `amd_MACHINE` and `amd_NOTSURE` extensions your campaign
needs. If the service cannot be reached, calls return `HUMAN` so no call is
lost.

To remove the module: add `--uninstall` to the command in step 3. Problems:
[Troubleshooting](#troubleshooting).

## Install

On the FreeSWITCH server, as root:

```bash
curl -fsSL https://raw.githubusercontent.com/nikvb/mod_amd_ws/main/install.sh | bash -s -- -y
```

The installer detects the running FreeSWITCH, installs the compiler and OpenSSL
headers, and finds the FreeSWITCH headers. If none are installed it tries the
distribution's dev package, then the matching source tarball from GitHub. It
then builds and checks the module and installs it with a backup of the old
one. Finally it creates `amd_ws.conf.xml` if absent, adds the module to
`modules.conf.xml` and loads it. Nothing is changed in the running switch
except `load` / `reload mod_amd_ws`.

```bash
curl -fsSL https://raw.githubusercontent.com/nikvb/mod_amd_ws/main/install.sh | bash -s -- --dry-run      # show the plan only
curl -fsSL https://raw.githubusercontent.com/nikvb/mod_amd_ws/main/install.sh | bash -s -- -y --host api.amdy.io --port 2700
curl -fsSL https://raw.githubusercontent.com/nikvb/mod_amd_ws/main/install.sh | bash -s -- --uninstall
```

All options: [docs/installer.md](docs/installer.md). Build by hand:
[Building](#building).

Verify:

```bash
fs_cli -x 'module_exists mod_amd_ws'      # true
fs_cli -x 'amd_ws status'                 # endpoint, settings, counters
fs_cli -x 'show application' | grep amd_ws
```

## Dialplan

```xml
<extension name="outbound_amd">
  <condition field="destination_number" expression="^amd$">
    <action application="amd_ws" data=""/>
    <action application="log" data="INFO AMD ${AMDSTATUS}/${AMDCAUSE} stats=${AMDSTATS}"/>
    <action application="execute_extension" data="amd_${AMDSTATUS} XML ${context}"/>
  </condition>
</extension>
```

With an empty argument the endpoint and timings come from `amd_ws.conf.xml`.
Full form, the same as Asterisk's `AMD_WS()`:

```text
amd_ws [host[,port[,vid[,timeout_ms[,playfile[,options]]]]]]
```

| Argument | Default | Meaning |
|---|---|---|
| `host` | `host` from `amd_ws.conf.xml` | Service host name or IP. |
| `port` | `port` from the config (2700) | TCP port. |
| `vid` | `${amd_ws_vid}`, else `${caller_id_name}`, else `Unknown` | Call id sent to the service as `VID`. |
| `timeout_ms` | `timeout_ms` from the config (10000) | Detection window from the first audio frame. |
| `playfile` | none | Played to the callee while detecting; stopped at the verdict. Any `playback` target (file, `tone_stream://`, `say:`...). |
| `options` | none | Letters, see below. |

If an argument contains commas (a `tone_stream://` spec, for example), use
FreeSWITCH's delimiter prefix and separate with `|`:

```xml
<action application="amd_ws" data="^^|api.amdy.io|2700||10000|tone_stream://%(500,500,440);loops=20|d(800)"/>
```

### Options

| Letter | Meaning |
|---|---|
| `s` | TLS (`wss://`). Certificate verification per `tls_verify`. |
| `d(ms)` | Start `playfile` after `ms` milliseconds. |
| `c(ms)` | Connect timeout (default `connect_timeout_ms`, 10000). |
| `p(phone)` `k(code)` `i(cid)` | Send `phone`, `country_code`, `caller_id` in the config frame. |
| `a` / `A` | Answer an unanswered channel (default) / do not answer. With `A` an unanswered channel gets `HUMAN`/`FATAL_ERROR`. |
| `v` | Trace this call at INFO: connect, each chunk, each reply, verdict, each with `+ms`. |
| `L` | Local detector only (no network; `mod_amd` behaviour). |
| `C` | Compare: stream **and** run the local detector; the service decides, the local verdict is reported. |
| `F` / `f` | Use the local verdict on `CONNECTION_ERROR`/`PROCESSING_ERROR` / do not (overrides `local_fallback`). |
| `n` | Accepted for Asterisk compatibility; no effect. |

### Channel variables read

`amd_ws_host`, `amd_ws_port`, `amd_ws_vid`, `amd_ws_timeout_ms`,
`amd_ws_connect_timeout_ms`, `amd_ws_tls`, `amd_ws_trace`, `amd_ws_mode`
(`ws`/`local`/`compare`), `amd_ws_local_fallback`, `amd_ws_compare_wait_local`,
`amd_ws_phone`, `amd_ws_country_code`, `amd_ws_caller_id`,
`amd_ws_playdelay_ms`. They override `amd_ws.conf.xml`. Dialplan arguments and
options override them. Set them in an originate string, for example
`{amd_ws_phone=3125551212,amd_ws_country_code=1}sofia/gateway/...`.

`caller_id` is sent automatically as the number the callee sees. The module
takes `effective_caller_id_number`, else `origination_caller_id_number`, else
`caller_id_number`. It never sends an empty value or `Unknown`, and it sends
nothing when `send_caller_id=false`.

### Channel variables set

| Variable | Value |
|---|---|
| `AMDSTATUS` | `HUMAN`, `MACHINE`, `NOTSURE` or `HANGUP`. |
| `AMDCAUSE` | See the table below. |
| `AMDSTATS` | `<elapsed_ms>-<audio_ms_sent>-<chunks_sent>-<bytes_sent>` on every exit. |
| `AMDRESPONSE` | Last text the service sent, printable ASCII, at most 255 characters. |
| `AMDELAPSED` | Milliseconds from the first captured audio frame to the verdict. |
| `amd_result`, `amd_cause` | Same as `AMDSTATUS` / `AMDCAUSE`, so dialplans written for `mod_amd` keep working. |
| `amd_ws_source` | `ws`, `local`, `fallback`, `timeout`, `error` or `hangup`. |
| `amd_ws_mode` | `ws`, `local` or `compare`. |
| `amd_ws_connect_ms` | WebSocket connect time (when connected). |
| `amd_ws_local_result`, `amd_ws_local_cause`, `amd_ws_local_ms` | Local detector verdict and when it decided (modes `local`/`compare`, or with fallback). |
| `amd_ws_fallback_from` | The service error that made the local verdict the result. |
| `AMDPHONE`, `AMDCOUNTRYCODE` | The phone / country code sent, when known. |

### Status and cause

| `AMDSTATUS` | `AMDCAUSE` | When |
|---|---|---|
| `HUMAN` | `HUMAN` | The service reply contains `HUMAN`. |
| `MACHINE` | the reply text (`MACHINE`, `AMD`, `AMD_DETECTED`, ...) | The reply contains `AMD` (not `AMDY`) or `MACHINE`. |
| `HUMAN` | `CONNECTION_ERROR` | DNS, TCP, TLS or upgrade failed, or took longer than `connect_timeout_ms`. |
| `HUMAN` | `PROCESSING_ERROR` | The connection broke or the server closed it before a verdict. |
| `HUMAN` | `FATAL_ERROR` | The module could not run (channel not answered with `A`, no media, out of resources). |
| `NOTSURE` | `SERVER_TIMEOUT` | `timeout_ms` passed with audio sent and no verdict. |
| `NOTSURE` | `NOAUDIODATA-<ms>` | `timeout_ms` passed and the channel delivered no audio at all. |
| `NOTSURE` | `EOF_INCONCLUSIVE` / `EOF_ERROR` | Media stopped after some speech; the service's answer to `{"eof":1}` had no verdict / no answer within `eof_wait_ms`. |
| `HANGUP` | `HANGUP` | The channel hung up during detection. |
| `NOTSURE` | `STOPPED` | Background detection stopped with `uuid_amd_ws <uuid> stop`. |
| as detected | `HUMAN`, `INITIALSILENCE`, `MAXWORDLENGTH`, `MAXWORDS`, `LONGGREETING`, `TOOLONG` | Local detector result (mode `local`, or the fallback). |

The three error causes return `HUMAN` so a call is never lost when the service
is down. Branch on them to use a local fallback in the dialplan:

```xml
<action application="execute_extension" data="${cond(${AMDCAUSE} == CONNECTION_ERROR ? amd_fallback : amd_${AMDSTATUS})} XML ${context}"/>
```

or let the module do it with `local_fallback=true` or option `F`.

## Background detection (bridge or IVR keeps running)

```bash
fs_cli -x 'uuid_amd_ws <uuid> start api.amdy.io,2700,,10000'
fs_cli -x 'uuid_amd_ws <uuid> stop'
```

The detection runs on a media bug while the channel does whatever it is doing:
a bridge, an IVR or playback. When the verdict is known, the module sets the
same variables and fires the event below. It also runs every channel variable
named `api_on_amd_ws_result*` as an API command. Its arguments are expanded
when it runs. `set` expands its value once more, so write `\\\${...}` for a
variable that must be expanded only when the command runs:

```xml
<action application="set" data="api_on_amd_ws_result=uuid_transfer ${uuid} amd_\\\${AMDSTATUS} XML default"/>
<action application="set" data="r=${uuid_amd_ws(${uuid} start)}"/>
<action application="bridge" data="..."/>
```

## Event

`CUSTOM amd_ws::result`, once per detection, with the channel's standard
headers plus:

`AMD-Status`, `AMD-Cause`, `AMD-Response`, `AMD-Source`, `AMD-Mode`,
`AMD-VID`, `AMD-Elapsed`, `AMD-Stats`, `AMD-Connect-MS`, `AMD-First-Audio-MS`,
`AMD-Background`, and when the local detector ran `AMD-Local-Status`,
`AMD-Local-Cause`, `AMD-Local-MS`, `AMD-Fallback-From`.

```bash
fs_cli -x '/event plain CUSTOM amd_ws::result'
```

## Configuration

`<conf_dir>/autoload_configs/amd_ws.conf.xml`. Every parameter, with its
default and meaning, is in [conf/amd_ws.conf.xml](conf/amd_ws.conf.xml). Apply
changes with `fs_cli -x 'amd_ws reload'`; calls in progress keep their settings.

| Parameter | Default | |
|---|---|---|
| `host`, `port`, `path` | `127.0.0.1` (sample: `api.amdy.io`), `2700`, `/` | Endpoint. |
| `tls`, `tls_verify`, `tls_check_hostname`, `tls_cafile` | `false`, `true`, `true`, system bundle | TLS. |
| `mode` | `ws` | `ws`, `local` or `compare`. |
| `local_fallback` | `false` | Use the local verdict on connection/processing errors. |
| `compare_wait_local` | `false` | Compare mode: return only after the local detector decided too. |
| `timeout_ms` | `10000` | Detection window. |
| `connect_timeout_ms` | `10000` | Connect limit. |
| `result_grace_ms` | `0` | Extra wait for a verdict after the window. |
| `send_schedule` | `500,1000,1500,2000,3000,4000,5000,6000,7000,8000,9000` | Send marks (ms). |
| `chunk_bytes`, `fallback_interval_ms` | `8000`, `1000` | Sends after the last mark. |
| `eof_no_audio_streak`, `eof_wait_ms` | `2`, `3000` | EOF finalisation when media stops. |
| `send_caller_id` | `true` | Send `caller_id`. |
| `extra_config` | empty | JSON object merged into the config frame. |
| `answer`, `playdelay_ms`, `trace` | `true`, `0`, `false` | Channel handling, logging. |
| `silence_threshold` ... `maximum_word_length` | `mod_amd` defaults | Local detector (same names as `amd.conf.xml`). |

## How it works

1. `amd_ws` answers the channel if needed and attaches a read-only media bug.
   Each frame the callee sends is resampled to 8 kHz if needed and handed to
   the call's engine. Frames the core fills because no RTP arrived count as
   "no audio".
2. A worker thread per call connects to `ws://host:port/`, sends the config
   frame and sends the audio collected at each schedule mark as one binary
   frame. It reads replies as they arrive and applies the `amd.py` rule:
   `HUMAN` in the text means human, then `AMD` or `MACHINE` means machine,
   anything else is an acknowledgement.
3. The channel thread only moves media, or runs the playback. It never waits
   on the network, so a slow or dead service cannot stall a call beyond
   `connect_timeout_ms` / `timeout_ms`.
4. On every exit the worker sends `{"eof":1}` and CLOSE 1000 and releases the
   socket.

Wire details: [docs/protocol.md](docs/protocol.md). Design:
[docs/architecture.md](docs/architecture.md).

## Native mod_amd vs streaming

[docs/performance.md](docs/performance.md) compares native energy detection
(`mod_amd` and the built-in local detector) with WebSocket streaming. It covers
decision latency, false positives and FreeSWITCH CPU, measured on real calls
with [bench/bench.py](bench/bench.py). Short version:

- **CPU is not the deciding factor.** Both cost well under 1 % of a core per
  concurrent call. Streaming costs more than local energy detection, because of
  a thread and a socket per call, but stays in that range.
- **Latency.** The local detector decides when its rules fire: about
  0.8 s after the end of "hello?", or at 1.5 s of continuous speech. Streaming
  decides when the service answers, at the 1.5 s or 2 s mark plus inference and
  RTT. The module adds a few milliseconds on top.
- **Accuracy is the deciding factor.** An energy detector counts words and
  silences. A long "Hello, this is John speaking", a quiet line, background
  noise, a short "leave a message" greeting and a late-starting voicemail each
  produce a wrong verdict by design. A classifier that listens to the content
  does not share those failure modes. Measure it on your own recordings with
  `bench/bench.py --corpus`.

## Building

```bash
make                                   # headers via pkg-config freeswitch
make FS_INCLUDES=-I/usr/local/freeswitch/include/freeswitch FS_LIBS="-L/usr/local/freeswitch/lib -lfreeswitch"
sudo make install                      # into the running switch's mod_dir (FS_MODDIR= to override)
make show-config                       # what fs-detect.sh found
make installer                         # regenerate install.sh after changing the sources
```

The module exports only `mod_amd_ws_module_interface` (linker version script).
All its internal symbols stay private, so they cannot clash with the core or
other modules. sofia-sip, for example, also has a `ws_close()`.

## Tests

```bash
make test-unit                                        # engine + WebSocket client vs the mock server, no FreeSWITCH
FS_BIN=/usr/bin/freeswitch python3 test/fs_test.py    # 32 scenarios on a real FreeSWITCH, as a normal user
python3 bench/bench.py --calls 400 --concurrency 100  # the comparison in docs/performance.md
```

`test/fs_test.py` starts a private FreeSWITCH from a scratch directory with its
own configuration. It uses the switch's modules through symlinks and never
touches the system configuration. Details: [test/README.md](test/README.md).

## Troubleshooting

```bash
fs_cli -x 'amd_ws status'                        # settings, calls in progress, worker threads, counters
fs_cli -x 'console loglevel debug'               # + trace=true or option v for per-call event lines
grep 'AMD_WS:' /var/log/freeswitch/freeswitch.log | tail
```

| Symptom | Check |
|---|---|
| Every call `HUMAN`/`CONNECTION_ERROR` | `curl -i -N -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' http://HOST:PORT/` from the switch; firewall; `host`/`port` in `amd_ws status`. The WARNING line names the reason (refused, timeout, TLS certificate, HTTP status). |
| `NOAUDIODATA-<ms>` | The channel delivered no media: early media / answer supervision, a codec with no decoder, or `bypass_media`. The module needs media through FreeSWITCH. |
| `unload mod_amd_ws` says "in use" | Calls are detecting. Unload/reload when idle; `amd_ws status` shows calls in progress. |
| `load` fails with undefined symbols | The module was built against headers of another FreeSWITCH version; rerun the installer on that host. |

## Files

| Path | |
|---|---|
| `src/mod_amd_ws.c` | FreeSWITCH glue: config, application, API, media bug, variables, event. |
| `src/amd_engine.c` | Per-call engine: worker thread, schedule, classifier, timeouts, local fallback. |
| `src/ws_client.c` | RFC 6455 client (ws/wss), non-blocking, every wait bounded. |
| `src/local_amd.c` | Energy/word-count detector (Asterisk `app_amd` via `mod_amd`). |
| `conf/amd_ws.conf.xml` | Sample configuration with every parameter. |
| `fs-detect.sh`, `Makefile` | Detection and build. |
| `install.sh` | Generated (`make installer`), embeds the sources. |
| `test/`, `bench/` | Integration tests, mock server, benchmark. |
