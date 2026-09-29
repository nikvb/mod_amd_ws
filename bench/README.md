# Benchmark

`bench.py` compares, on a private FreeSWITCH with loopback calls, the decision
latency, the verdicts and the FreeSWITCH CPU and memory of five modes:

- `baseline` (no detection);
- `mod_amd`, if `mod_amd.so` is in the module directory;
- `amd_ws` local detector (`L`);
- `amd_ws` streaming;
- `amd_ws` compare (`C`).

Results and their interpretation: [docs/performance.md](../docs/performance.md).
The raw output of the runs it quotes is in `results/`.

```bash
python3 bench/bench.py --calls 400 --concurrency 100 --json results/c100.json
python3 bench/bench.py --corpus ~/labelled --ws-host api.amdy.io --modes local,ws,compare
```

| Option | Default | |
|---|---|---|
| `--calls` | 200 | calls per mode (after 10 warm-up calls) |
| `--concurrency` | 50 | calls in flight |
| `--modes` | `baseline,mod_amd,local,ws,compare` | |
| `--corpus DIR` | generated synthetic corpus | `human_*.wav`, `machine_*.wav`, 8 kHz mono 16-bit |
| `--ws-host`, `--ws-port` | the mock in oracle mode | a real service instead |
| `--ws-after N` | 2 | oracle: verdict with chunk N+1 (2 → the 1500 ms mark) |
| `--ws-reply-delay MS` | 0 | oracle: delay every reply (models inference + RTT) |
| `--fs-bin`, `--fs-mods`, `--module` | `freeswitch` in PATH, its mod dir, `./mod_amd_ws.so` | |
| `--json FILE` | | machine-readable results |

With the oracle mock the streaming verdict is the file's label by
construction. Its accuracy columns say *oracle* and measure nothing but the
plumbing. Streaming accuracy is the service's accuracy; measure it with
`--ws-host` on real recordings. A real service receives the corpus audio;
the synthetic corpus is speech-shaped noise, not speech.
