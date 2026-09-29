#!/usr/bin/env python3
"""Benchmark: native FreeSWITCH mod_amd vs mod_amd_ws (local detector, WebSocket stream).

Measures, on a real FreeSWITCH, with N concurrent loopback calls per mode:
  * decision latency   CHANNEL_EXECUTE -> CHANNEL_EXECUTE_COMPLETE of the AMD application
  * verdicts           per labelled corpus file -> confusion matrix / false-positive rate
  * media-server CPU   utime+stime of the freeswitch process, per call and per concurrent call,
                       after subtracting the same calls with no detection ("baseline")
  * memory             RSS of the freeswitch process at the end of each mode

Modes:
  baseline   the far side plays the file; the AMD side just waits (sleep) -> cost of the calls themselves
  mod_amd    seanbright/mod_amd "amd" application (Asterisk app_amd algorithm), if mod_amd.so is available
  local      amd_ws with option L (the same algorithm inside mod_amd_ws)
  ws         amd_ws streaming to a WebSocket server (default: the mock in "oracle" mode, see below)
  compare    amd_ws option C (stream + local detector in parallel)

The WebSocket server: by default the bundled mock (test/mock_amd_server.py) answers with the
corpus label after --ws-after acknowledged chunks (2 -> verdict with the 3rd chunk, 1500 ms of
audio), optionally delayed by --ws-reply-delay ms.  That isolates what the MODULE costs; it
says nothing about the service's accuracy (the verdict is the label by construction, reported
as "oracle").  --ws-host/--ws-port point the ws and compare modes at a real service instead;
then the verdicts and latencies are the service's (it receives the synthetic corpus audio).

Usage:
  bench/bench.py [--calls 200] [--concurrency 50] [--modes baseline,mod_amd,local,ws,compare]
                 [--corpus DIR] [--ws-host H --ws-port P] [--ws-after 2] [--ws-reply-delay MS]
                 [--fs-bin PATH] [--fs-mods DIR] [--module PATH] [--json out.json]
A corpus directory holds 8 kHz mono 16-bit WAV files named human_*.wav / machine_*.wav.
"""
import argparse
import collections
import json
import os
import platform
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "test"))
import fs_test  # noqa: E402  (ESL, mock helpers)


def cpu_ticks(pid):
    with open("/proc/%d/stat" % pid) as f:
        parts = f.read().rsplit(")", 1)[1].split()
    return int(parts[11]) + int(parts[12])      # utime + stime (clock ticks)


def rss_kb(pid):
    with open("/proc/%d/status" % pid) as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    return 0


def fs_pid(proc):
    """PID of the freeswitch process (Popen may be the process itself)."""
    return proc.pid


def write_config(d, esl_port, corpus, files, ws_host, ws_port, have_mod_amd):
    os.makedirs(os.path.join(d, "conf"), exist_ok=True)
    mods = ["mod_console", "mod_logfile", "mod_event_socket", "mod_commands", "mod_dptools", "mod_dialplan_xml",
            "mod_loopback", "mod_sndfile", "mod_tone_stream", "mod_amd_ws"] + (["mod_amd"] if have_mod_amd else [])
    far = "\n".join(
        '<extension name="f_%s"><condition field="destination_number" expression="^f_%s$">'
        '<action application="answer"/><action application="playback" data="%s"/><action application="park"/>'
        '</condition></extension>' % (n, n, os.path.join(corpus, n + ".wav")) for n in files)
    sides = {
        "baseline": '<action application="sleep" data="2500"/>',
        "mod_amd": '<action application="amd" data=""/>',
        "local": '<action application="amd_ws" data="127.0.0.1,1,${bench_vid},10000,,L"/>',
        "ws": '<action application="amd_ws" data="%s,%d,${bench_vid},10000"/>' % (ws_host, ws_port),
        "compare": '<action application="amd_ws" data="%s,%d,${bench_vid},10000,,C"/>' % (ws_host, ws_port),
    }
    amd = "\n".join(
        '<extension name="a_%s"><condition field="destination_number" expression="^a_%s$">%s'
        '<action application="hangup"/></condition></extension>' % (m, m, x) for m, x in sides.items())
    conf = """<?xml version="1.0"?>
<document type="freeswitch/xml">
 <section name="configuration">
  <configuration name="switch.conf"><settings>
    <param name="max-sessions" value="5000"/><param name="sessions-per-second" value="1000"/>
    <param name="loglevel" value="warning"/>
  </settings></configuration>
  <configuration name="modules.conf"><modules>%s</modules></configuration>
  <configuration name="console.conf"><mappings><map name="all" value="console,warning,err,crit,alert"/></mappings>
    <settings><param name="loglevel" value="warning"/></settings></configuration>
  <configuration name="logfile.conf"><settings/><profiles><profile name="default"><settings>
    <param name="logfile" value="%s/freeswitch.log"/><param name="rollover" value="0"/></settings>
    <mappings><map name="all" value="info,notice,warning,err,crit,alert"/></mappings></profile></profiles></configuration>
  <configuration name="event_socket.conf"><settings>
    <param name="listen-ip" value="127.0.0.1"/><param name="listen-port" value="%d"/><param name="password" value="ClueCon"/>
  </settings></configuration>
  <configuration name="amd_ws.conf"><settings><param name="tls_verify" value="false"/></settings></configuration>
  <configuration name="amd.conf"><settings/></configuration>
 </section>
 <section name="dialplan"><context name="bench">
   %s
   %s
 </context></section>
</document>
""" % ("".join('<load module="%s"/>' % m for m in mods), d, esl_port, far, amd)
    with open(os.path.join(d, "conf/freeswitch.xml"), "w") as f:
        f.write(conf)


def pct(xs, p):
    if not xs:
        return None
    xs = sorted(xs)
    k = (len(xs) - 1) * p / 100.0
    lo, hi = int(k), min(int(k) + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def run_mode(esl, mode, files, calls, conc, pid, app_name):
    """Run `calls` calls, `conc` at a time.  Returns per-call records and resource deltas."""
    recs = {}
    pending = collections.deque()
    for i in range(calls):
        pending.append((i, files[i % len(files)]))
    inflight = {}
    t_start = time.time()
    c0 = cpu_ticks(pid)
    seen = 0
    deadline = time.time() + calls * 15 / max(conc, 1) + 120

    def launch(i, fname):
        u = esl.api("create_uuid").strip()
        vid = "B-%s-%d" % (fname, i)
        esl.api("bgapi originate {origination_uuid=%s,bench_vid=%s,verbose_events=true,ignore_early_media=true}"
                "loopback/f_%s/bench a_%s XML bench" % (u, vid, fname, mode))
        inflight[u] = {"i": i, "file": fname, "label": fname.split("_")[0], "t_orig": time.time()}

    while pending and len(inflight) < conc:
        launch(*pending.popleft())

    while (inflight or pending) and time.time() < deadline:
        with esl.cv:
            evs = esl.events[seen:]
            seen = len(esl.events)
        for ev in evs:
            u = ev.get("Unique-ID")
            if u not in inflight:
                continue
            r = inflight[u]
            name = ev.get("Event-Name")
            app = ev.get("Application")
            ts = int(ev.get("Event-Date-Timestamp", "0")) / 1e6
            if name == "CHANNEL_EXECUTE" and app == app_name:
                r["t_exec"] = ts
            elif name == "CHANNEL_EXECUTE_COMPLETE" and app == app_name:
                r["t_done"] = ts
                r["status"] = ev.get("variable_AMDSTATUS") or ev.get("variable_amd_result") or "-"
                r["cause"] = ev.get("variable_AMDCAUSE") or ev.get("variable_amd_cause") or "-"
                r["elapsed"] = ev.get("variable_AMDELAPSED")
                r["local"] = ev.get("variable_amd_ws_local_result")
            elif name == "CHANNEL_HANGUP_COMPLETE":
                r["t_hup"] = ts
                recs[u] = inflight.pop(u)
                if pending:
                    launch(*pending.popleft())
        with esl.cv:
            if len(esl.events) == seen:
                esl.cv.wait(0.05)
        if seen > 20000:            # keep memory flat
            with esl.cv:
                del esl.events[:seen]
                seen = 0
    wall = time.time() - t_start
    c1 = cpu_ticks(pid)
    for u in list(inflight):
        esl.api("uuid_kill " + u)
    time.sleep(1.0)
    hz = os.sysconf("SC_CLK_TCK")
    call_seconds = sum((r.get("t_hup", 0) - r["t_orig"]) for r in recs.values() if r.get("t_hup"))
    return {
        "records": list(recs.values()),
        "lost": len(inflight),
        "cpu_s": (c1 - c0) / hz,
        "wall_s": wall,
        "call_seconds": call_seconds,
        "rss_kb": rss_kb(pid),
    }


def summarise(mode, res, baseline_rate, conc, oracle):
    recs = [r for r in res["records"] if "t_done" in r and "t_exec" in r]
    lat = [(r["t_done"] - r["t_exec"]) * 1000 for r in recs]
    by_label = collections.defaultdict(list)
    for r in recs:
        by_label[r["label"]].append(r)
    conf = collections.Counter((r["label"], r["status"]) for r in recs)
    humans = [r for r in recs if r["label"] == "human"]
    machines = [r for r in recs if r["label"] == "machine"]
    fp = sum(1 for r in humans if r["status"] == "MACHINE")          # human sent to the machine path
    fn = sum(1 for r in machines if r["status"] == "HUMAN")          # machine sent to an agent
    ns = sum(1 for r in recs if r["status"] not in ("HUMAN", "MACHINE"))
    extra_cpu = res["cpu_s"] - (baseline_rate * res["call_seconds"] if baseline_rate is not None else 0)
    out = {
        "mode": mode,
        "calls": len(recs),
        "lost": res["lost"] + (len(res["records"]) - len(recs)),
        "latency_ms": {"p50": pct(lat, 50), "p90": pct(lat, 90), "p99": pct(lat, 99), "max": max(lat) if lat else None},
        "latency_ms_by_label": {k: pct([(r["t_done"] - r["t_exec"]) * 1000 for r in v], 50) for k, v in by_label.items()},
        "false_machine_rate": (fp / len(humans)) if humans else None,
        "false_human_rate": (fn / len(machines)) if machines else None,
        "notsure": ns,
        "oracle": oracle,
        "cpu_s": res["cpu_s"],
        "cpu_ms_per_call_total": 1000 * res["cpu_s"] / max(len(recs), 1),
        "cpu_ms_per_call_detection": (1000 * extra_cpu / max(len(recs), 1)) if baseline_rate is not None else None,
        "cpu_pct_avg": 100 * res["cpu_s"] / max(res["wall_s"], 1e-9),
        "cpu_pct_per_concurrent_call_detection": (100 * extra_cpu / max(res["wall_s"], 1e-9) / conc) if baseline_rate is not None else None,
        "rss_mb": res["rss_kb"] / 1024.0,
        "confusion": {"%s->%s" % k: v for k, v in sorted(conf.items())},
        "per_file": {f: collections.Counter(r["status"] + "/" + r["cause"] for r in recs if r["file"] == f)
                     for f in sorted(set(r["file"] for r in recs))},
    }
    return out


def fmt(x, nd=0):
    if x is None:
        return "-"
    return ("%%.%df" % nd) % x


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--calls", type=int, default=200)
    ap.add_argument("--concurrency", type=int, default=50)
    ap.add_argument("--modes", default="baseline,mod_amd,local,ws,compare")
    ap.add_argument("--corpus")
    ap.add_argument("--ws-host")
    ap.add_argument("--ws-port", type=int, default=2700)
    ap.add_argument("--ws-after", type=int, default=2)
    ap.add_argument("--ws-reply-delay", type=int, default=0)
    ap.add_argument("--fs-bin", default=os.environ.get("FS_BIN") or shutil.which("freeswitch") or "freeswitch")
    ap.add_argument("--fs-mods", default=os.environ.get("FS_MODS"))
    ap.add_argument("--module", default=os.path.join(ROOT, "mod_amd_ws.so"))
    ap.add_argument("--json")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()

    mods = a.fs_mods or fs_test.find_mod_dir(a.fs_bin)
    d = tempfile.mkdtemp(prefix="amdws-bench-")
    corpus = a.corpus
    if not corpus:
        subprocess.check_call([sys.executable, os.path.join(ROOT, "test", "gen_audio.py"), os.path.join(d, "audio")],
                              stdout=subprocess.DEVNULL)
        corpus = os.path.join(d, "audio", "bench")
    files = sorted(f[:-4] for f in os.listdir(corpus) if f.endswith(".wav") and f.split("_")[0] in ("human", "machine"))
    if not files:
        sys.exit("no human_*.wav / machine_*.wav in %s" % corpus)

    moddir = os.path.join(d, "mod")
    os.makedirs(moddir)
    for f in os.listdir(mods):
        if f.endswith(".so") and f != "mod_amd_ws.so":
            os.symlink(os.path.join(mods, f), os.path.join(moddir, f))
    os.symlink(os.path.abspath(a.module), os.path.join(moddir, "mod_amd_ws.so"))
    have_mod_amd = os.path.exists(os.path.join(moddir, "mod_amd.so"))

    procs = []
    oracle = not a.ws_host
    if oracle:
        path = "/oracle?after=%d%s" % (a.ws_after, "&reply=%d" % a.ws_reply_delay if a.ws_reply_delay else "")
        mock, ws_port, ctl, _rec = fs_test.start_mock(d, "mock", [])
        open(ctl, "w").write(path + "\n")
        procs.append(mock)
        ws_host = "127.0.0.1"
    else:
        ws_host, ws_port = a.ws_host, a.ws_port

    esl_port = fs_test.free_port()
    write_config(d, esl_port, corpus, files, ws_host, ws_port, have_mod_amd)
    for sub in ("log", "db", "run"):
        os.makedirs(os.path.join(d, sub), exist_ok=True)
    fs = subprocess.Popen([a.fs_bin, "-nonat", "-nf", "-nc", "-conf", os.path.join(d, "conf"), "-log", os.path.join(d, "log"),
                           "-db", os.path.join(d, "db"), "-run", os.path.join(d, "run"), "-temp", os.path.join(d, "run"),
                           "-mod", moddir], stdout=open(os.path.join(d, "fs.stdout"), "w"), stderr=subprocess.STDOUT,
                          start_new_session=True)
    esl = None
    for _ in range(300):
        try:
            esl = fs_test.ESL("127.0.0.1", esl_port, "ClueCon")
            break
        except OSError:
            time.sleep(0.1)
    if not esl:
        sys.exit("FreeSWITCH did not start; see %s" % d)
    while "UP" not in esl.api("status"):
        time.sleep(0.1)
    esl.subscribe("CHANNEL_EXECUTE CHANNEL_EXECUTE_COMPLETE CHANNEL_HANGUP_COMPLETE")
    pid = fs_pid(fs)
    version = esl.api("version").strip()
    cpu_model = ""
    try:
        cpu_model = [l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name")][0]
    except (OSError, IndexError):
        pass
    print("FreeSWITCH: %s\nhost: %s, %d CPUs; corpus: %d files (%s); calls/mode %d, concurrency %d; ws: %s"
          % (version, cpu_model, os.cpu_count(), len(files), corpus, a.calls, a.concurrency,
             ("mock oracle, verdict after %d acks, reply delay %d ms" % (a.ws_after, a.ws_reply_delay)) if oracle
             else "%s:%d" % (ws_host, ws_port)), flush=True)

    apps = {"baseline": "sleep", "mod_amd": "amd", "local": "amd_ws", "ws": "amd_ws", "compare": "amd_ws"}
    results = []
    baseline_rate = None
    for mode in a.modes.split(","):
        if mode == "mod_amd" and not have_mod_amd:
            print("%-9s skipped: mod_amd.so not in %s" % (mode, mods))
            continue
        # warm-up
        run_mode(esl, mode, files, min(len(files), 10), min(a.concurrency, 10), pid, apps[mode])
        res = run_mode(esl, mode, files, a.calls, a.concurrency, pid, apps[mode])
        if mode == "baseline":
            baseline_rate = res["cpu_s"] / max(res["call_seconds"], 1e-9)
        s = summarise(mode, res, baseline_rate if mode != "baseline" else None, a.concurrency,
                      oracle and mode in ("ws", "compare"))
        results.append(s)
        print("%-9s done: %d calls, lost %d, p50 %s ms, CPU %.1f s" % (mode, s["calls"], s["lost"],
              fmt(s["latency_ms"]["p50"]), s["cpu_s"]), flush=True)

    # report
    print("\n| mode | calls | latency p50 / p90 / p99 ms | human p50 | machine p50 | false MACHINE (humans) | false HUMAN (machines) | NOTSURE | CPU ms per call (detection) | CPU % per concurrent call | RSS MB |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for s in results:
        orc = " (oracle)" if s["oracle"] else ""
        print("| %s | %d | %s / %s / %s | %s | %s | %s%s | %s%s | %d | %s | %s | %.0f |" % (
            s["mode"], s["calls"], fmt(s["latency_ms"]["p50"]), fmt(s["latency_ms"]["p90"]), fmt(s["latency_ms"]["p99"]),
            fmt(s["latency_ms_by_label"].get("human")), fmt(s["latency_ms_by_label"].get("machine")),
            "-" if s["false_machine_rate"] is None or s["mode"] == "baseline" else "%.0f%%" % (100 * s["false_machine_rate"]), orc if s["mode"] != "baseline" else "",
            "-" if s["false_human_rate"] is None or s["mode"] == "baseline" else "%.0f%%" % (100 * s["false_human_rate"]), orc if s["mode"] != "baseline" else "",
            s["notsure"] if s["mode"] != "baseline" else 0,
            fmt(s["cpu_ms_per_call_detection"], 2), fmt(s["cpu_pct_per_concurrent_call_detection"], 3), s["rss_mb"]))
    print("\nVerdicts per corpus file:")
    for s in results:
        if s["mode"] == "baseline":
            continue
        print("  %s:" % s["mode"])
        for f, c in s["per_file"].items():
            print("    %-24s %s" % (f, ", ".join("%s x%d" % kv for kv in c.most_common())))
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"freeswitch": version, "cpu": cpu_model, "cpus": os.cpu_count(), "platform": platform.platform(),
                       "calls": a.calls, "concurrency": a.concurrency, "oracle": oracle, "results": results}, f, indent=1, default=dict)
        print("\nwrote", a.json)

    try:
        esl.api("fsctl shutdown", timeout=3)
    except Exception:
        pass
    try:
        fs.wait(20)
    except subprocess.TimeoutExpired:
        os.killpg(fs.pid, signal.SIGKILL)
    for p in procs:
        p.terminate()
    if not a.keep:
        shutil.rmtree(d, ignore_errors=True)
    else:
        print("kept", d)


if __name__ == "__main__":
    main()
