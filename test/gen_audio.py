#!/usr/bin/env python3
"""Generate synthetic 8 kHz 16-bit mono slin (.sln) and .wav test files.

Speech-like bursts: noise shaped with a 4 Hz syllable envelope, level ~ -18 dBFS.
They exercise timing and the energy detector; they are NOT a substitute for
real call recordings when measuring accuracy (see bench/README.md).

  human.sln    0.3 s silence, "hello?" 0.7 s, 4 s silence
  human2.sln   0.2 s silence, 1.1 s speech, 4 s silence
  machine.sln  0.4 s silence, 6 words of 0.5 s with 0.12 s gaps, 1.5 s tone-ish speech (greeting ~5 s)
  longword.sln 0.3 s silence, 6 s continuous speech
  silence.sln  8 s digital silence
  hello_then_nothing.sln  1.2 s speech only (the caller then stops sending media)
"""
import math, os, random, struct, sys, wave

RATE = 8000
random.seed(7)

def silence(sec):
    return [0] * int(RATE * sec)

def speech(sec, amp=4000):
    n = int(RATE * sec); out = []; prev = 0.0
    for i in range(n):
        t = i / RATE
        env = 0.55 + 0.45 * math.sin(2 * math.pi * 4 * t)      # syllables
        x = random.uniform(-1, 1)
        prev = 0.6 * prev + 0.4 * x                              # low-pass
        s = amp * env * (prev * 1.6 + 0.5 * math.sin(2 * math.pi * 180 * t))
        out.append(max(-32767, min(32767, int(s))))
    # 10 ms fades
    f = int(RATE * 0.01)
    for i in range(min(f, n)):
        out[i] = int(out[i] * i / f); out[-1 - i] = int(out[-1 - i] * i / f)
    return out

def words(count, wlen, gap):
    out = []
    for _ in range(count):
        out += speech(wlen) + silence(gap)
    return out

def noise(sec, amp):
    return [int(random.uniform(-amp, amp)) for _ in range(int(RATE * sec))]

def mix(a, b):
    n = max(len(a), len(b)); a = a + [0] * (n - len(a)); b = b + [0] * (n - len(b))
    return [max(-32767, min(32767, x + y)) for x, y in zip(a, b)]

def tone(sec, hz, amp=8000):
    return [int(amp * math.sin(2 * math.pi * hz * i / RATE)) for i in range(int(RATE * sec))]

# Labelled bench corpus (bench/README.md): typical cases plus the known hard
# cases for energy-based detection.  bench/<label>_<name>.wav
BENCH = {
    "human_hello": silence(0.4) + speech(0.6) + silence(5.0),
    "human_hello_2words": silence(0.3) + speech(0.4) + silence(0.15) + speech(0.5) + silence(5.0),
    "human_late_hello": silence(1.6) + speech(0.6) + silence(5.0),
    "human_long_greeting": silence(0.3) + speech(2.2) + silence(5.0),          # "Hello, this is John Smith speaking"
    "human_quiet": silence(0.3) + speech(0.7, amp=250) + silence(5.0),          # low-level line
    "human_noisy": mix(silence(0.3) + speech(0.7) + silence(5.0), noise(6.0, 700)),  # street / call-centre noise
    "machine_greeting": silence(0.5) + words(5, 0.55, 0.15) + speech(1.2) + silence(0.3) + tone(0.8, 1000) + silence(2.0),
    "machine_long_word": silence(0.4) + speech(4.0) + tone(0.8, 1000) + silence(1.0),
    "machine_short": silence(0.3) + speech(0.9) + silence(0.9) + tone(0.8, 1000) + silence(3.0),  # "Leave a message." beep
    "machine_silent_start": silence(3.0) + words(3, 0.5, 0.2) + silence(2.0),   # carrier voicemail that starts late
}

FILES = {
    "human": silence(0.3) + speech(0.7) + silence(4.0),
    "human2": silence(0.2) + speech(1.1) + silence(4.0),
    "machine": silence(0.4) + words(6, 0.5, 0.12) + speech(1.5) + silence(1.0),
    "longword": silence(0.3) + speech(6.0) + silence(1.0),
    "silence": silence(8.0),
    "hello_then_nothing": speech(1.2),
}

def main(outdir):
    os.makedirs(outdir, exist_ok=True)
    for name, s in FILES.items():
        raw = struct.pack("<%dh" % len(s), *s)
        open(os.path.join(outdir, name + ".sln"), "wb").write(raw)
        with wave.open(os.path.join(outdir, name + ".wav"), "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(raw)
    bdir = os.path.join(outdir, "bench")
    os.makedirs(bdir, exist_ok=True)
    for name, s in BENCH.items():
        with wave.open(os.path.join(bdir, name + ".wav"), "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
            w.writeframes(struct.pack("<%dh" % len(s), *s))
    print("wrote", len(FILES), "files to", outdir, "and", len(BENCH), "to", bdir)

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "audio"))
