# Native mod_amd vs WebSocket stream detection: production performance

This page compares native FreeSWITCH energy detection with streaming the audio
to a detection service over WebSocket. Native detection means the `mod_amd`
application and the same algorithm built into `mod_amd_ws` (option `L`);
streaming means `mod_amd_ws`. It covers three things:

- **decision latency**, the time from the start of detection to the verdict on
  the channel;
- **false positives**, humans sent to the machine path and machines sent to agents;
- **media-server cost**, the CPU and memory of the FreeSWITCH process, plus bandwidth.

Everything below was measured with [`bench/bench.py`](../bench/bench.py) on
real FreeSWITCH calls. The raw output is in [`bench/results/`](../bench/results/).
Rerun it on your hardware and your recordings; the commands are at the end.

## Summary

| | Native energy detection (`mod_amd`, `amd_ws` option `L`) | WebSocket stream (`amd_ws`) |
|---|---|---|
| How it decides | Counts words and silences by frame energy; fixed rules (`initial_silence`, `greeting`, `after_greeting_silence`, `maximum_number_of_words`, ...). | A service classifies the audio content; the module forwards it at 500, 1000, 1500, 2000, 3000 ms ... and applies the reply. |
| Latency (this test) | p50 ≈ 2.0 s: a human is confirmed only after 800 ms of silence following "hello". A voicemail is flagged after 3 words or 1.5 s of speech. p99 up to 4.9 s (`TOOLONG`). | The verdict arrives when the service answers. With a verdict at the 1500 ms mark: p50 1.56-1.58 s, p99 ≤ 1.82 s at 200 concurrent calls. Add the service's inference time and the network round trip. |
| Module overhead on the verdict | none (in-process) | `AMDELAPSED` 1500-1502 ms for a reply sent at the 1500 ms mark: 1-2 ms |
| False positives | Structural. Long human greetings, quiet lines, background noise and short or late voicemail greetings are misclassified by design (table below). | Those of the service. The module adds none: it passes the service's verdict through, and on errors it returns `HUMAN`, or the local verdict with `local_fallback`. |
| FreeSWITCH CPU per detection | ≈ 5-7 ms (0.15-0.26 % of one core per concurrent call) | ≈ 8-9 ms (0.38-0.50 % of one core per concurrent call) |
| FreeSWITCH memory | ≈ 0 extra | ≈ 0.27 MB per concurrent detection (worker thread + buffers) |
| Network | none | 16 kB/s up per detecting call (128 kbit/s + framing), a few bytes down; one TCP (TLS) connection per call |
| Failure mode | none external | Service unreachable → `HUMAN`/`CONNECTION_ERROR` within `connect_timeout_ms` (immediately when refused); optional local fallback. |

**Bottom line.** Neither approach is expensive for the media server. On a 2012
1.9 GHz Xeon, streaming 200 simultaneous detections costs well under one core.
The difference that matters in production is **accuracy**. An energy detector
has failure modes you cannot tune away. A content classifier does not share
them, and it can also decide sooner. Measure the service on your own labelled
recordings; [How to reproduce](#how-to-reproduce) shows how. Use `compare`
mode on live traffic to see both verdicts side by side before switching.

## Test setup

| | |
|---|---|
| Host | Intel Xeon E5-2420 @ 1.90 GHz, 12 CPUs, Ubuntu 22.04 |
| FreeSWITCH | 1.10.12 (source build), `mod_amd_ws` 1.0.0, `mod_amd` = github.com/seanbright/mod_amd d3cfaaf |
| Calls | `mod_loopback`: the far leg plays a corpus WAV (8 kHz), the near leg runs the detector. 400 calls per mode at 100 concurrent, 600 per mode at 200 concurrent, round-robin over the corpus. |
| Latency | `CHANNEL_EXECUTE` → `CHANNEL_EXECUTE_COMPLETE` of the detection application (event timestamps, µs). |
| CPU | `utime + stime` of the freeswitch process over each mode. "Per detection" subtracts the cost of the same calls without detection (mode `baseline`, scaled by call-seconds). |
| WebSocket server | The bundled mock in *oracle* mode, on the same host. It acknowledges chunks 1 and 2 and answers the 3rd chunk (the 1500 ms mark) with the corpus label. This isolates what the **module** costs. Its accuracy figures are the label by construction and are marked *oracle*. |
| Corpus | 10 synthetic WAVs: 6 human, 4 machine (`test/gen_audio.py`). Speech-like noise bursts with a 4 Hz syllable envelope. They reproduce the timing and energy patterns that energy detection depends on. They are **not** real speech; see [False positives](#false-positives). |

## Results

200 concurrent calls, 600 calls per mode (`bench/results/c200.txt`):

| mode | latency p50 / p90 / p99 ms | false MACHINE (humans) | false HUMAN (machines) | CPU ms per detection | CPU % of a core per concurrent call | RSS MB |
|---|---|---|---|---|---|---|
| baseline (no detection) | 2510 / 2520 / 2520 | - | - | - | - | 178 |
| `mod_amd` (seanbright) | 1960 / 3564 / 4900 | 80 % | 2 % | 4.7 | 0.15 | 178 |
| `amd_ws` local (`L`) | 2050 / 2566 / 3020 | 50 % | 25 % | 5.8 | 0.20 | 193 |
| `amd_ws` stream | 1580 / 1620 / 1700 | 0 % (oracle) | 0 % (oracle) | 8.1 | 0.38 | 231 |
| `amd_ws` compare (`C`) | 1560 / 1601 / 1819 | 0 % (oracle) | 0 % (oracle) | 8.0 | 0.38 | 232 |

100 concurrent calls, 400 per mode (`bench/results/c100.txt`):

| mode | latency p50 / p90 / p99 ms | false MACHINE | false HUMAN | CPU ms per detection | CPU % per concurrent call | RSS MB |
|---|---|---|---|---|---|---|
| baseline | 2500 / 2520 / 2520 | - | - | - | - | 111 |
| `mod_amd` | 2000 / 3182 / 4900 | 88 % | 2 % | 6.1 | 0.21 | 111 |
| local (`L`) | 2040 / 2548 / 3000 | 50 % | 25 % | 6.8 | 0.26 | 122 |
| stream | 1560 / 1580 / 1600 | 0 % (oracle) | 0 % (oracle) | 8.2 | 0.45 | 138 |
| compare | 1560 / 1560 / 1580 | 0 % (oracle) | 0 % (oracle) | 9.3 | 0.50 | 140 |

No calls were lost in any run. The CPU per detection is small next to the calls
themselves: a baseline loopback call with playback costs 24-27 ms of CPU.
Subtracting the baseline leaves a noise of about ±1 ms.

## Latency

**Energy detection** decides only when one of its rules fires:

| Callee | Rule | Verdict after (defaults) |
|---|---|---|
| Says "hello?" then waits | `after_greeting_silence` | end of the word + 800 ms (≈ 1.8-2.1 s into the call) |
| Voicemail greeting | `maximum_number_of_words` (3) or `greeting` (1500 ms of speech) | third word or 1.5 s of continuous speech (≈ 1.7-2.1 s) |
| Silence | `initial_silence` | 2.5 s |
| Anything undecided | `total_analysis_time` | 5 s → `NOTSURE`/`TOOLONG` |

The 800 ms wait after a human's "hello" is the cost of certainty for this
method. A shorter `after_greeting_silence` answers sooner but calls a human
who pauses mid-sentence a machine.

**Streaming** decides as soon as the service does. The service receives
500 ms of audio at 500 ms, 1000 ms, 1500 ms and 2000 ms, then one second at a
time. A verdict with the third chunk reaches the channel at 1500 ms + inference
+ network round trip. The module itself adds 1-2 ms: `AMDELAPSED` was 1500-1502
for a reply sent at the 1500 ms mark. The channel thread never waits on the
network. The worker thread polls the socket and the channel reads the verdict
on its next 20 ms frame, and in background mode the event fires on that frame.
The p99 under load (1.7-1.8 s at 200 concurrent calls) includes the Python
mock and 400 loopback channels sharing the same host. This test does not
separate that from the module's own share.

Parallel playback (`playfile` argument) does not change detection timing. The
prompt stops within one frame of the verdict.

## False positives

What the synthetic corpus shows:

| File | What it models | `mod_amd` (seanbright), 60 calls | local detector / fixed `mod_amd` | Correct |
|---|---|---|---|---|
| human_hello | "hello?" then waiting | MACHINE 53, HUMAN 6, NOTSURE 1 | HUMAN | HUMAN |
| human_hello_2words | "hello, hello?" | MACHINE 57, HUMAN 3 | HUMAN | HUMAN |
| human_late_hello | answers after 1.6 s | MACHINE 49, HUMAN 11 | HUMAN | HUMAN |
| human_long_greeting | "Hello, this is John Smith speaking" (2.2 s) | MACHINE 58, HUMAN 2 | MACHINE (LONGGREETING) | HUMAN |
| human_noisy | "hello" over street/call-centre noise | HUMAN 42, MACHINE 18 | MACHINE (LONGGREETING) | HUMAN |
| human_quiet | "hello" on a very low-level line | MACHINE 52, HUMAN 6, NOTSURE 2 | MACHINE (INITIALSILENCE) | HUMAN |
| machine_greeting | multi-word greeting + beep | MACHINE 60 | MACHINE (MAXWORDS) | MACHINE |
| machine_long_word | continuous speech 4 s | MACHINE 60 | MACHINE (LONGGREETING) | MACHINE |
| machine_short | "Leave a message." + beep | MACHINE 55, HUMAN 5 | HUMAN | MACHINE |
| machine_silent_start | carrier voicemail that starts after 3 s | MACHINE 59, HUMAN 1 | MACHINE (INITIALSILENCE) | MACHINE |

Two separate findings:

1. **The energy algorithm has fixed blind spots.** Its correct
   implementation is Asterisk `app_amd`, the local detector in `mod_amd_ws`,
   or `mod_amd` with the bug below fixed. It still gets 4 of these 10 files
   wrong, the same way on every call:
   - a human who says more than a short word is a MACHINE (`LONGGREETING` / `MAXWORDS`);
   - a quiet line is a MACHINE (`INITIALSILENCE`);
   - background noise never reads as silence, so the "silence after greeting"
     never arrives and the verdict is MACHINE;
   - a short voicemail greeting looks exactly like "hello?" and is a HUMAN.

   You can move these errors between the two columns by changing the
   thresholds. You cannot remove them, because energy and timing are all the
   algorithm sees. On real traffic, how often each case occurs depends on your
   lists and carriers. That is why the numbers above are not accuracy rates:
   the corpus deliberately contains the hard cases.

2. **The widely used FreeSWITCH `mod_amd` (github.com/seanbright/mod_amd) has
   an energy bug.** Its frame loop is:

   ```c
   for (energy = 0, j = 0, count = 0; count < f->samples; count++) {
       energy += abs(audio[j++]);
       j += codec->number_of_channels;
   }
   ```

   On a mono channel this adds every second sample and reads twice the frame
   length, past the end of the audio into whatever the buffer held before.
   Two effects follow on this corpus. It calls 80-88 % of humans MACHINE, even
   a plain "hello?". And its verdicts are not repeatable: the same file got
   different results on different calls. With the one-line fix
   (`energy += abs(audio[j]); j += codec->number_of_channels;`) it produced
   exactly the local detector's verdicts on every file. If you use `mod_amd`
   today, `amd_ws` with option `L` gives you the algorithm as intended, with
   the same parameter names.

**Streaming accuracy** is the accuracy of the service, and a synthetic corpus
cannot measure it. Measure it on real recordings, human and machine, from your
own traffic:

```bash
python3 bench/bench.py --corpus /path/to/labelled --ws-host api.amdy.io --ws-port 2700 --modes local,ws
```

The corpus directory holds `human_*.wav` and `machine_*.wav`, 8 kHz mono
16-bit. On live traffic, `compare` mode (`mode=compare` or option `C`) records
both verdicts for every call without changing call handling. The service's
verdict decides, and `amd_ws_local_result` and the event carry the local one,
so you can count disagreements per campaign before you rely on either.

## Media-server cost in production terms

The FreeSWITCH process cost measured above, scaled to a dialer. This is the
worst case from the runs, on a slow 2012 CPU; a current CPU is roughly twice
as fast per core.

| Concurrent detections | Energy detection | Streaming | Streaming bandwidth up |
|---|---|---|---|
| 50 | ≈ 0.1 core | ≈ 0.25 core, +14 MB | 6.4 Mbit/s |
| 200 | ≈ 0.5 core | ≈ 1 core, +54 MB | 26 Mbit/s |
| 500 | ≈ 1.3 core | ≈ 2.5 cores, +135 MB | 64 Mbit/s |

Concurrent detections are not concurrent calls. A detection lasts about 1.5-3 s
at the start of an answered call, so 200 concurrent detections is a very large
dialer.

Where the streaming cost goes: one detached worker thread per detection, one
TCP connection, a JSON config frame and 5-6 binary frames. `wss://` adds a TLS
handshake per call, which this test did not measure. Each call opens its own
connection, so there is no TLS session reuse. Bandwidth is fixed by the
protocol: 8 kHz 16-bit mono, 16 000 bytes per second while detecting.

What the module does **not** cost:

- **No change to the call's media.** The media bug is read-only
  (`SMBF_READ_STREAM`). As with call recording or `mod_amd`, a media bug makes
  the core decode the callee's audio to linear PCM while it is attached; this
  test used L16, so that decode is not in its figures (G.711 decoding is a
  table lookup per sample). The module adds no encoding.
- **No blocking of the channel.** The network is on the worker thread. A slow
  or dead service costs `connect_timeout_ms` at most, and only for that call.
- **No libwebsockets or libks.** The WebSocket client is one C file on
  OpenSSL, which FreeSWITCH already loads.

## Resilience

| Situation | Energy detection | Streaming |
|---|---|---|
| Service down (connection refused) | n/a | `HUMAN`/`CONNECTION_ERROR` at once (0-10 ms), or the local verdict with `local_fallback`. |
| Service host black-holed | n/a | `HUMAN`/`CONNECTION_ERROR` at `connect_timeout_ms` (tested: 1.46 s with `c(1500)`). |
| Service accepts but never answers | n/a | `NOTSURE`/`SERVER_TIMEOUT` at `timeout_ms`. |
| Service closes mid-call | n/a | `HUMAN`/`PROCESSING_ERROR` at once. |
| Callee's media stops after "hello" | `HUMAN` after 800 ms of "silence" (the core's comfort-noise fill) | `{"eof":1}` after two empty marks; the service decides from what it heard. |
| Callee hangs up | `amd` ends | `HANGUP`/`HANGUP` at once; `{"eof":1}` + CLOSE 1000 to the service. |

Every one of these is a scenario in `test/fs_test.py`.

## Recommendation

1. **Stream for accuracy.** Use `amd_ws` in `ws` mode against the service and
   keep the ViciDial-style fallback branch on `CONNECTION_ERROR` /
   `PROCESSING_ERROR` / `FATAL_ERROR`, or set `local_fallback=true`.
2. **Prove it on your traffic first.** Run `mode=compare` on a slice of
   campaigns for a few days. Then compare `AMDSTATUS` with
   `amd_ws_local_result` and with agent dispositions.
3. **Replace `mod_amd`** with `amd_ws ... L` if you need a network-free
   detector. It is the same algorithm, without the energy bug.
4. **Size the uplink** at 128 kbit/s per concurrent detection. Set
   `connect_timeout_ms` to 2000-3000 if you would rather send calls to the
   fallback quickly than wait out a slow DNS lookup.

## How to reproduce

```bash
make                                              # or: install.sh --build-only
python3 bench/bench.py --calls 600 --concurrency 200 --json out.json         # this page, mock oracle
python3 bench/bench.py --ws-reply-delay 150                                   # model 150 ms inference + RTT
python3 bench/bench.py --corpus ~/labelled --ws-host api.amdy.io --modes local,ws,compare   # real service, real audio
```

`bench.py` needs FreeSWITCH with `mod_loopback`, `mod_sndfile`,
`mod_dptools`, `mod_commands` and `mod_event_socket`. It runs its own private
instance from a scratch directory as a normal user. `mod_amd` is compared when
`mod_amd.so` is in the module directory. Run the benchmark on an idle machine,
because the CPU figures are differences between runs.
