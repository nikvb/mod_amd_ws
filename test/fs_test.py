#!/usr/bin/env python3
"""Integration tests for mod_amd_ws against a real FreeSWITCH.

Runs as a normal user.  Nothing is installed into the system: a scratch
directory gets a generated configuration, a module directory of symlinks
(the switch's own modules + the freshly built mod_amd_ws.so), the mock AMD
server(s) and a black-hole listener.  Every scenario is one loopback call:

    originate {vars}loopback/far_<farside>/amdtest  amdside XML amdtest

The b-leg ("far side", the callee) plays a recording and then parks (no
media -> CNG), plays silence or hangs up; the a-leg runs amd_ws (or
uuid_amd_ws in background) and the harness checks the CUSTOM amd_ws::result
event plus what the mock server recorded on the wire.

Usage:
  test/fs_test.py [--fs-bin PATH] [--fs-mods DIR] [--module mod_amd_ws.so]
                  [--only name,name] [--list] [--keep] [-v]
Environment: FS_BIN, FS_MODS (defaults: freeswitch in PATH, its mod_dir).
Exit status 0 = all scenarios passed.
"""
import argparse
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# name, farside, mock path ('-' = none), amd_ws data (placeholders {port} {dead} {hole} {tls}),
# expected status, cause regex, min_ms, max_ms, extra checks
SCENARIOS = [
    ("human",         "human",      "/human?after=2",   "127.0.0.1,{port},V-human,10000",            "HUMAN",   r"^HUMAN$",            1300, 2600, ["proto", "stats"]),
    ("machine",       "machine",    "/machine?after=1", "127.0.0.1,{port},V-machine,10000",          "MACHINE", r"^MACHINE$",          800, 2000, ["stats"]),
    ("amd_detected",  "machine",    "/status?value=AMD_DETECTED&after=1", "127.0.0.1,{port},V-amdd,10000", "MACHINE", r"^AMD_DETECTED$", 800, 2000, []),
    ("json_human",    "human",      "/json",            "127.0.0.1,{port},V-json,10000",             "HUMAN",   r"^HUMAN$",            0, 3000, ["resp:status"]),
    ("amdy_guard",    "machine",    "/amdy",            "127.0.0.1,{port},V-amdy,10000",             "HUMAN",   r"^HUMAN$",            1300, 5000, []),
    ("fragmented",    "human",      "/fragmented",      "127.0.0.1,{port},V-frag,10000",             "HUMAN",   r"^HUMAN$",            0, 3000, []),
    ("ping",          "human",      "/ping?after=2",    "127.0.0.1,{port},V-ping,10000",             "HUMAN",   r"^HUMAN$",            0, 3000, []),
    ("eof_human",     "hello_park", "/eof_human",       "127.0.0.1,{port},V-eofh,10000",             "HUMAN",   r"^HUMAN$",            2700, 4200, ["eof"]),
    ("eof_ack",       "hello_park", "/eof_ack",         "127.0.0.1,{port},V-eofa,10000",             "NOTSURE", r"^EOF_INCONCLUSIVE$", 2700, 4200, []),
    ("eof_silent",    "hello_park", "/eof_silent",      "127.0.0.1,{port},V-eofs,10000",             "NOTSURE", r"^EOF_ERROR$",        5700, 7300, []),
    ("server_timeout","human_long", "/silent",          "127.0.0.1,{port},V-sto,3000",               "NOTSURE", r"^SERVER_TIMEOUT$",   2900, 4000, []),
    ("noaudio",       "park",       "/silent",          "127.0.0.1,{port},V-noaudio,3000",           "NOTSURE", r"^NOAUDIODATA-3000$", 2900, 4000, ["stats"]),
    ("conn_refused",  "human",      "-",                "127.0.0.1,{dead},V-refused,10000",          "HUMAN",   r"^CONNECTION_ERROR$", 0, 600, ["noconn"]),
    ("blackhole",     "human",      "-",                "127.0.0.1,{hole},V-hole,10000,,c(1500)",    "HUMAN",   r"^CONNECTION_ERROR$", 1400, 2600, []),
    ("rejected",      "human",      "/reject",          "127.0.0.1,{port},V-reject,10000",           "HUMAN",   r"^CONNECTION_ERROR$", 0, 800, []),
    ("server_close",  "machine",    "/close?after=1",   "127.0.0.1,{port},V-close,10000",            "HUMAN",   r"^PROCESSING_ERROR$", 800, 2200, []),
    ("hangup",        "hello_hangup","/silent",         "127.0.0.1,{port},V-hangup,10000",           "HANGUP",  r"^HANGUP$",           900, 2200, []),
    ("playback",      "human",      "/human?after=2",   "^^|127.0.0.1|{port}|V-play|10000|tone_stream://%(500,500,440);loops=20", "HUMAN", r"^HUMAN$", 1300, 2800, []),
    ("play_delay",    "human",      "/human?after=2",   "^^|127.0.0.1|{port}|V-pdel|10000|tone_stream://%(500,500,440);loops=20|d(800)", "HUMAN", r"^HUMAN$", 1300, 2800, []),
    ("config_keys",   "human",      "/human?after=2",   "127.0.0.1,{port},V-cfg,10000,,p(3125551212)k(1)i(3125550100)", "HUMAN", r"^HUMAN$", 0, 3000,
        ["cfg:phone=3125551212", "cfg:country_code=1", "cfg:caller_id=3125550100"]),
    ("local_human",   "human",      "-",                "127.0.0.1,{dead},V-lh,10000,,L",            "HUMAN",   r"^HUMAN$",            1500, 2600, ["noconn", "source:local"]),
    ("local_machine", "machine",    "-",                "127.0.0.1,{dead},V-lm,10000,,L",            "MACHINE", r"^(MAXWORDS|LONGGREETING)$", 1000, 3200, ["noconn"]),
    ("local_silence", "park",       "-",                "127.0.0.1,{dead},V-ls,10000,,L",            "MACHINE", r"^INITIALSILENCE$",   2300, 3300, []),
    ("compare",       "machine",    "/machine?after=1", "127.0.0.1,{port},V-cmp,10000,,C",           "MACHINE", r"^MACHINE$",          800, 2000, ["local"]),
    ("compare_wait",  "machine",    "/machine?after=1", "127.0.0.1,{port},V-cmpw,10000,,C",          "MACHINE", r"^MACHINE$",          800, 2000, ["localdone", "var:amd_ws_compare_wait_local=true"]),
    ("fallback",      "machine",    "-",                "127.0.0.1,{dead},V-fb,10000,,F",            "MACHINE", r"^(MAXWORDS|LONGGREETING)$", 1000, 3200, ["fallback"]),
    ("tls",           "human",      "tls:/human?after=2", "127.0.0.1,{tls},V-tls,10000,,s",          "HUMAN",   r"^HUMAN$",            1300, 2800, []),
    ("wideband",      "human",      "/human?after=2",   "127.0.0.1,{port},V-wb,10000",               "HUMAN",   r"^HUMAN$",            1300, 2800, ["stats", "bytes8k"]),
    ("background",    "human",      "/human?after=2",   "BG:127.0.0.1,{port},V-bg,10000",            "HUMAN",   r"^HUMAN$",            0, 3500, ["bg"]),
    ("bg_stop",       "human",      "/silent",          "BGSTOP:127.0.0.1,{port},V-bgs,10000",       "NOTSURE", r"^STOPPED$",          0, 3500, ["bg"]),
    ("concurrency",   "human",      "/human?after=2",   "MULTI20:127.0.0.1,{port},V-multi,10000",    "HUMAN",   r"^HUMAN$",            1300, 4000, []),
]

FARSIDES = {
    # name: dialplan actions for the b-leg
    "human": ["answer", "playback:{audio}/human.wav", "park"],
    "human_long": ["answer", "playback:{audio}/human.wav", "playback:{audio}/human2.wav", "playback:{audio}/human.wav", "park"],
    "machine": ["answer", "playback:{audio}/machine.wav", "park"],
    "hello_park": ["answer", "playback:{audio}/hello_then_nothing.wav", "park"],
    "hello_hangup": ["answer", "playback:{audio}/hello_then_nothing.wav", "hangup"],
    "park": ["answer", "park"],
}


# ---------------------------------------------------------------------------
class ESL:
    """Tiny inbound event-socket client (plain events)."""

    def __init__(self, host, port, password):
        self.s = socket.create_connection((host, port), timeout=10)
        self.buf = b""
        self.lock = threading.Lock()
        self.events = []
        self.cv = threading.Condition()
        hdr, _ = self._read_msg()
        assert hdr.get("Content-Type") == "auth/request", hdr
        self._send("auth " + password)
        hdr, _ = self._read_msg()
        assert hdr.get("Reply-Text", "").startswith("+OK"), hdr
        self.replies = []
        self.reader = threading.Thread(target=self._reader, daemon=True)
        self.reader.start()

    def _send(self, cmd):
        self.s.sendall((cmd + "\n\n").encode())

    def _read_until(self, sep):
        while sep not in self.buf:
            d = self.s.recv(65536)
            if not d:
                raise EOFError
            self.buf += d
        i = self.buf.index(sep)
        out, self.buf = self.buf[:i], self.buf[i + len(sep):]
        return out

    def _read_n(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d:
                raise EOFError
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _read_msg(self):
        raw = self._read_until(b"\n\n").decode(errors="replace")
        hdr = {}
        for line in raw.split("\n"):
            if ": " in line:
                k, v = line.split(": ", 1)
                hdr[k] = urllib.parse.unquote(v)
        body = b""
        if "Content-Length" in hdr:
            body = self._read_n(int(hdr["Content-Length"]))
        return hdr, body.decode(errors="replace")

    def _reader(self):
        self.s.settimeout(None)
        try:
            while True:
                hdr, body = self._read_msg()
                ct = hdr.get("Content-Type")
                with self.cv:
                    if ct == "text/event-plain":
                        ev = {}
                        head, _, evbody = body.partition("\n\n")
                        for line in head.split("\n"):
                            if ": " in line:
                                k, v = line.split(": ", 1)
                                ev[k] = urllib.parse.unquote(v)
                        if evbody:
                            ev["_body"] = evbody
                        ev["_t"] = time.time()
                        self.events.append(ev)
                    else:
                        self.replies.append((hdr, body))
                    self.cv.notify_all()
        except (EOFError, OSError):
            pass

    def api(self, cmd, timeout=15):
        with self.cv:
            n = len(self.replies)
            self._send("api " + cmd)
            end = time.time() + timeout
            while len(self.replies) <= n and time.time() < end:
                self.cv.wait(0.2)
            return self.replies[n][1] if len(self.replies) > n else ""

    def subscribe(self, what):
        with self.cv:
            n = len(self.replies)
            self._send("event plain " + what)
            end = time.time() + 5
            while len(self.replies) <= n and time.time() < end:
                self.cv.wait(0.2)

    def wait_event(self, pred, timeout):
        end = time.time() + timeout
        with self.cv:
            while True:
                for ev in self.events:
                    if pred(ev):
                        return ev
                left = end - time.time()
                if left <= 0:
                    return None
                self.cv.wait(min(left, 0.2))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def find_mod_dir(fs_bin):
    for d in ("/usr/lib/freeswitch/mod", "/usr/lib64/freeswitch/mod", "/usr/local/freeswitch/mod",
              os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(fs_bin))), "lib/freeswitch/mod")):
        if os.path.exists(os.path.join(d, "mod_dptools.so")):
            return d
    return None


# ---------------------------------------------------------------------------
def write_config(d, esl_port, audio, tls_cafile):
    os.makedirs(os.path.join(d, "conf/autoload_configs"), exist_ok=True)
    mods = ["mod_console", "mod_logfile", "mod_event_socket", "mod_commands", "mod_dptools", "mod_dialplan_xml",
            "mod_loopback", "mod_sndfile", "mod_native_file", "mod_tone_stream", "mod_amd_ws"]

    def act(a):
        app, _, data = a.partition(":")
        data = data.replace("{audio}", audio)
        return '<action application="%s" data="%s"/>' % (app, data.replace('"', "&quot;"))

    far = "\n".join(
        '<extension name="far_%s"><condition field="destination_number" expression="^far_%s$">%s</condition></extension>'
        % (n, n, "".join(act(a) for a in acts)) for n, acts in FARSIDES.items())
    conf = """<?xml version="1.0"?>
<document type="freeswitch/xml">
 <section name="configuration">
  <configuration name="switch.conf"><settings>
    <param name="max-sessions" value="2000"/><param name="sessions-per-second" value="500"/>
    <param name="loglevel" value="debug"/><param name="rtp-start-port" value="30000"/><param name="rtp-end-port" value="30100"/>
  </settings></configuration>
  <configuration name="modules.conf"><modules>%s</modules></configuration>
  <configuration name="console.conf"><mappings><map name="all" value="console,debug,info,notice,warning,err,crit,alert"/></mappings>
    <settings><param name="colorize" value="false"/><param name="loglevel" value="info"/></settings></configuration>
  <configuration name="logfile.conf"><settings><param name="rotate-on-hup" value="true"/></settings>
    <profiles><profile name="default"><settings><param name="logfile" value="%s/freeswitch.log"/><param name="rollover" value="0"/></settings>
    <mappings><map name="all" value="debug,info,notice,warning,err,crit,alert"/></mappings></profile></profiles></configuration>
  <configuration name="event_socket.conf"><settings>
    <param name="listen-ip" value="127.0.0.1"/><param name="listen-port" value="%d"/><param name="password" value="ClueCon"/>
  </settings></configuration>
  <configuration name="amd_ws.conf"><settings>
    <param name="host" value="127.0.0.1"/><param name="port" value="1"/>
    <param name="tls_verify" value="true"/><param name="tls_cafile" value="%s"/><param name="tls_check_hostname" value="true"/>
    <param name="trace" value="true"/>
  </settings></configuration>
 </section>
 <section name="dialplan">
  <context name="amdtest">
   %s
   <extension name="amdside"><condition field="destination_number" expression="^amdside$">
     <action application="set" data="amd_t0=${strepoch()}"/>
     <action application="amd_ws" data="${amdargs}"/>
     <action application="hangup"/>
   </condition></extension>
   <extension name="bgside"><condition field="destination_number" expression="^bgside$">
     <action application="set" data="api_on_amd_ws_result=uuid_setvar ${uuid} amd_api_ran \\\\\\${AMDSTATUS}"/>
     <action application="set" data="bg_start=${uuid_amd_ws(${uuid} start ${amdargs})}"/>
     <action application="playback" data="silence_stream://6000"/>
     <action application="hangup"/>
   </condition></extension>
  </context>
 </section>
</document>
""" % ("".join('<load module="%s"/>' % m for m in mods), d, esl_port, tls_cafile or "", far)
    with open(os.path.join(d, "conf/freeswitch.xml"), "w") as f:
        f.write(conf)


def make_cert(d):
    key, crt = os.path.join(d, "tls.key"), os.path.join(d, "tls.crt")
    if not shutil.which("openssl"):
        return None, None
    r = subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=127.0.0.1",
                        "-addext", "subjectAltName=IP:127.0.0.1", "-keyout", key, "-out", crt],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return (key, crt) if r.returncode == 0 else (None, None)


def start_mock(d, name, extra):
    ctl = os.path.join(d, name + ".ctl")
    open(ctl, "w").write("/human?after=2\n")
    pf = os.path.join(d, name + ".port")
    rec = os.path.join(d, name + ".jsonl")
    p = subprocess.Popen([sys.executable, os.path.join(HERE, "mock_amd_server.py"), "--port", "0", "--port-file", pf,
                          "--control", ctl, "--record", rec] + extra,
                         stdout=open(os.path.join(d, name + ".log"), "w"), stderr=subprocess.STDOUT)
    for _ in range(100):
        if os.path.exists(pf) and open(pf).read().strip():
            return p, int(open(pf).read().strip()), ctl, rec
        time.sleep(0.1)
    raise RuntimeError("mock server %s did not start" % name)


def mock_records(rec, vid):
    out = []
    if not os.path.exists(rec):
        return out
    for line in open(rec):
        try:
            o = json.loads(line)
        except ValueError:
            continue
        if o.get("event") == "connection" and o.get("vid") == vid:
            out.append(o)
    return out


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fs-bin", default=os.environ.get("FS_BIN") or shutil.which("freeswitch") or "freeswitch")
    ap.add_argument("--fs-mods", default=os.environ.get("FS_MODS"))
    ap.add_argument("--module", default=os.path.join(ROOT, "mod_amd_ws.so"))
    ap.add_argument("--only")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    if a.list:
        for s in SCENARIOS:
            print(s[0])
        return 0
    only = set(a.only.split(",")) if a.only else None

    mods = a.fs_mods or find_mod_dir(a.fs_bin)
    if not mods:
        print("cannot find the FreeSWITCH module directory; pass --fs-mods", file=sys.stderr)
        return 2
    d = tempfile.mkdtemp(prefix="amdws-fs-")
    audio = os.path.join(d, "audio")
    subprocess.check_call([sys.executable, os.path.join(HERE, "gen_audio.py"), audio], stdout=subprocess.DEVNULL)
    moddir = os.path.join(d, "mod")
    os.makedirs(moddir)
    for f in os.listdir(mods):
        if f.endswith(".so") and f != "mod_amd_ws.so":
            os.symlink(os.path.join(mods, f), os.path.join(moddir, f))
    os.symlink(os.path.abspath(a.module), os.path.join(moddir, "mod_amd_ws.so"))

    procs = []
    key, crt = make_cert(d)
    mock, port, ctl, rec = start_mock(d, "mock", [])
    procs.append(mock)
    tls_port = tls_ctl = tls_rec = None
    if crt:
        tmock, tls_port, tls_ctl, tls_rec = start_mock(d, "tlsmock", ["--tls-cert", crt, "--tls-key", key])
        procs.append(tmock)
    hole_pf = os.path.join(d, "hole.port")
    hole = subprocess.Popen([sys.executable, os.path.join(HERE, "blackhole_server.py"), "--port", "0", "--port-file", hole_pf],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    procs.append(hole)
    for _ in range(50):
        if os.path.exists(hole_pf) and open(hole_pf).read().strip():
            break
        time.sleep(0.1)
    hole_port = int(open(hole_pf).read().strip())
    dead_port = free_port()

    esl_port = free_port()
    write_config(d, esl_port, audio, crt)
    for sub in ("log", "db", "run"):
        os.makedirs(os.path.join(d, sub), exist_ok=True)
    gdb = ["gdb", "-batch", "-ex", "handle SIGPIPE nostop noprint", "-ex", "run", "-ex", "thread apply all bt 12", "--args"] if os.environ.get("FS_GDB") else []
    fs = subprocess.Popen(gdb + [a.fs_bin, "-nonat", "-nf", "-nc", "-np", "-conf", os.path.join(d, "conf"),
                           "-log", os.path.join(d, "log"), "-db", os.path.join(d, "db"), "-run", os.path.join(d, "run"),
                           "-temp", os.path.join(d, "run"), "-mod", moddir],
                          stdout=open(os.path.join(d, "fs.stdout"), "w"), stderr=subprocess.STDOUT, start_new_session=True)
    esl = None
    for _ in range(300):
        try:
            esl = ESL("127.0.0.1", esl_port, "ClueCon")
            break
        except OSError:
            if fs.poll() is not None:
                break
            time.sleep(0.1)
    if not esl:
        print("FreeSWITCH did not come up; see", d, file=sys.stderr)
        return 2
    for _ in range(100):
        if "UP" in esl.api("status"):
            break
        time.sleep(0.1)
    loaded = esl.api("module_exists mod_amd_ws").strip()
    print("FreeSWITCH:", esl.api("version").strip())
    print("mod_amd_ws loaded:", loaded, " scratch:", d)
    if loaded != "true":
        print(open(os.path.join(d, "log/freeswitch.log")).read()[-3000:])
        return 2
    esl.subscribe("CUSTOM amd_ws::result")

    results = []
    for (name, farside, mockpath, data, exp_st, exp_cause, tmin, tmax, checks) in SCENARIOS:
        if only and name not in only:
            continue
        kind = "app"
        count = 1
        if data.startswith("BGSTOP:"):
            kind, data = "bgstop", data[7:]
        elif data.startswith("BG:"):
            kind, data = "bg", data[3:]
        elif data.startswith("MULTI"):
            m = re.match(r"MULTI(\d+):(.*)", data)
            count, data = int(m.group(1)), m.group(2)
        data = data.format(port=port, dead=dead_port, hole=hole_port, tls=tls_port or dead_port)
        if mockpath.startswith("tls:"):
            if not tls_port:
                results.append((name, "SKIP", "no openssl for the TLS mock"))
                continue
            open(tls_ctl, "w").write(mockpath[4:] + "\n")
            use_rec = tls_rec
        else:
            if mockpath != "-":
                open(ctl, "w").write(mockpath + "\n")
            use_rec = rec
        vid = data[3:].split("|")[2] if data.startswith("^^|") else data.split(",")[2]
        vars_ = "amdargs=%s,ignore_early_media=true,origination_caller_id_number=3125550100" % data.replace(",", "\\,")
        for c in checks:
            if c.startswith("var:"):
                vars_ += "," + c[4:]
        if name == "wideband":
            vars_ += ",loopback_initial_codec=L16@16000h"
        dest = "bgside" if kind in ("bg", "bgstop") else "amdside"
        t_before = time.time()
        uuids = []
        for i in range(count):
            u = esl.api("create_uuid").strip()
            uuids.append(u)
            esl.api("bgapi originate {origination_uuid=%s,%s}loopback/far_%s/amdtest %s XML amdtest" % (u, vars_, farside, dest))
        if kind == "bgstop":
            time.sleep(1.5)
            r = esl.api("uuid_amd_ws %s stop" % uuids[0]).strip()
            if not r.startswith("+OK"):
                results.append((name, "FAIL", "stop: " + r))
                continue
        evs = []
        for u in uuids:
            ev = esl.wait_event(lambda e, u=u: e.get("Unique-ID") == u, (tmax / 1000.0) + 12)
            evs.append(ev)
        if kind == "bg" and evs[0]:
            time.sleep(0.3)
            ran = esl.api("uuid_getvar %s amd_api_ran" % uuids[0]).strip()
            if ran != evs[0].get("AMD-Status"):
                checks = checks + ["__apion_fail:%s" % ran]
        # tidy up
        for u in uuids:
            esl.api("uuid_kill %s" % u)
        fails = []
        for u, ev in zip(uuids, evs):
            if not ev:
                fails.append("no amd_ws::result event for %s" % u)
                continue
            st, cause = ev.get("AMD-Status"), ev.get("AMD-Cause", "")
            el = int(ev.get("AMD-Elapsed", "-1"))
            wall = (ev["_t"] - t_before) * 1000
            if st != exp_st:
                fails.append("status %s != %s (cause %s)" % (st, exp_st, cause))
            if not re.search(exp_cause, cause):
                fails.append("cause %r !~ %s" % (cause, exp_cause))
            # the AMD clock starts at the first audio frame; allow the far side's answer lag
            if el > tmax or wall > tmax + 1500:
                fails.append("too slow: elapsed %d wall %d > %d" % (el, wall, tmax))
            if el < tmin - 100:
                fails.append("too fast: elapsed %d < %d" % (el, tmin))
            stats = ev.get("AMD-Stats", "")
            if not re.match(r"^\d+-\d+-\d+-\d+$", stats):
                fails.append("AMDSTATS %r" % stats)
            else:
                e_, ams, ch, by = map(int, stats.split("-"))
                if "stats" in checks and (ams * 16 != by - by % 16 or e_ != el):
                    fails.append("AMDSTATS inconsistent %s" % stats)
                if "bytes8k" in checks and ams > el + 600:
                    fails.append("audio_ms %d > elapsed %d (resampling?)" % (ams, el))
            if "local" in checks and not ev.get("AMD-Local-Status"):
                fails.append("no AMD-Local-Status in compare mode")
            if "localdone" in checks and ev.get("AMD-Local-Status") not in ("HUMAN", "MACHINE", "NOTSURE"):
                fails.append("local verdict not waited for: %r" % ev.get("AMD-Local-Status"))
            if "fallback" in checks and ev.get("AMD-Fallback-From") != "CONNECTION_ERROR":
                fails.append("AMD-Fallback-From %r" % ev.get("AMD-Fallback-From"))
            if "bg" in checks and ev.get("AMD-Background") != "true":
                fails.append("not a background event")
            for c in checks:
                if c.startswith("__apion_fail:"):
                    fails.append("api_on_amd_ws_result did not run (amd_api_ran=%r)" % c[13:])
                if c.startswith("source:") and ev.get("AMD-Source") != c[7:]:
                    fails.append("source %r != %s" % (ev.get("AMD-Source"), c[7:]))
                if c.startswith("resp:") and c[5:] not in ev.get("AMD-Response", ""):
                    fails.append("AMDRESPONSE %r lacks %s" % (ev.get("AMD-Response"), c[5:]))
        time.sleep(0.4)   # let the worker send {"eof":1}/CLOSE and the mock write its record
        recs = mock_records(use_rec, vid)
        if "noconn" in checks and recs:
            fails.append("mock saw a connection")
        if ("proto" in checks or "eof" in checks or any(c.startswith("cfg:") for c in checks)) and not recs:
            fails.append("mock has no record for %s" % vid)
        for r in recs[:1]:
            cfg = (r.get("config") or {}).get("config", {})
            if "proto" in checks:
                if list(cfg.keys())[:2] != ["sample_rate", "VID"] or cfg.get("sample_rate") != 8000 or cfg.get("VID") != vid:
                    fails.append("config frame %s" % r.get("config_raw"))
                if r.get("close_code") != 1000:
                    fails.append("close code %s" % r.get("close_code"))
                if not r.get("eof"):
                    fails.append("no {\"eof\":1} on exit")
                if cfg.get("caller_id") != "3125550100":
                    fails.append("caller_id %r" % cfg.get("caller_id"))
            if "eof" in checks and len(r.get("eofs") or []) < 2:
                fails.append("expected EOF finalisation + exit eof, got %s" % r.get("eofs"))
            for c in checks:
                if c.startswith("cfg:"):
                    k, v = c[4:].split("=", 1)
                    if str(cfg.get(k)) != v:
                        fails.append("config %s=%r, want %s" % (k, cfg.get(k), v))
            if "cfg:phone=3125551212" in checks and list(cfg.keys()) != ["sample_rate", "VID", "phone", "country_code", "caller_id"]:
                fails.append("config key order %s" % list(cfg.keys()))
        detail = "; ".join(fails) if fails else "status=%s cause=%s elapsed=%s stats=%s%s" % (
            evs[0].get("AMD-Status"), evs[0].get("AMD-Cause"), evs[0].get("AMD-Elapsed"), evs[0].get("AMD-Stats"),
            (" local=%s/%s@%s" % (evs[0].get("AMD-Local-Status"), evs[0].get("AMD-Local-Cause"), evs[0].get("AMD-Local-MS"))
             if evs[0].get("AMD-Local-Status") else "") + (" x%d" % count if count > 1 else ""))
        results.append((name, "FAIL" if fails else "PASS", detail))
        print("%-15s %s  %s" % (name, results[-1][1], detail), flush=True)

    # module lifecycle: unload refused while a call runs, reload/unload/load when idle
    if not only or "lifecycle" in only:
        fails = []
        open(ctl, "w").write("/silent\n")
        u = esl.api("create_uuid").strip()
        esl.api("bgapi originate {origination_uuid=%s,amdargs=127.0.0.1\\,%d\\,V-life\\,6000}loopback/far_human/amdtest amdside XML amdtest" % (u, port))
        time.sleep(1.5)
        r = esl.api("unload mod_amd_ws")
        if "+OK" in r:
            fails.append("unload succeeded during a call: %r" % r.strip())
        esl.api("uuid_kill %s" % u)
        time.sleep(1.0)
        st = esl.api("amd_ws status")
        if "worker threads        0" not in st:
            fails.append("workers still alive after the call: %s" % st)
        r = esl.api("reload mod_amd_ws")
        if "+OK" not in r:
            fails.append("reload failed: %r" % r.strip())
        if esl.api("module_exists mod_amd_ws").strip() != "true":
            fails.append("module missing after reload")
        results.append(("lifecycle", "FAIL" if fails else "PASS", "; ".join(fails) or "unload refused in use; reload ok"))
        print("%-15s %s  %s" % results[-1], flush=True)

    if a.verbose:
        print(esl.api("amd_ws status"))

    # teardown
    try:
        esl.api("fsctl shutdown", timeout=3)
    except Exception:
        pass
    try:
        fs.wait(15)
    except subprocess.TimeoutExpired:
        os.killpg(fs.pid, signal.SIGKILL)
    for p in procs:
        p.terminate()
    crashed = fs.returncode not in (0, None, -15)
    npass = sum(1 for r in results if r[1] == "PASS")
    nfail = sum(1 for r in results if r[1] == "FAIL")
    nskip = sum(1 for r in results if r[1] == "SKIP")
    print("\n%d passed, %d failed, %d skipped%s" % (npass, nfail, nskip, "; FreeSWITCH exit %s" % fs.returncode if crashed else ""))
    if nfail or crashed or a.keep:
        print("scratch kept:", d)
    else:
        shutil.rmtree(d, ignore_errors=True)
    return 1 if (nfail or crashed) else 0


if __name__ == "__main__":
    sys.exit(main())
