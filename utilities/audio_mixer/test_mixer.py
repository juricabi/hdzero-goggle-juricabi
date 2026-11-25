#!/usr/bin/env python3
"""Host test of the audio mixer in juricabi's firmware: ripples' real settings_load() (src/core/settings.c),
then the real src/core/audio_mixer.c and audio_mixer_routing.c (worker, setting.ini through minIni,
the batches written for amixer) against a model of the codec's signal graph (codec_model.py, from
the driver's DAPM routes).

    utilities/audio_mixer/test_mixer.py [--src DIR] [--driver sun8iw16-codec.c] [-v]

For every target (Goggle, Goggle 2, BoxPro):
  routing     a matrix of mixer states: which sources reach the recording (left, right) and
              the headphones, through which gains; every batch line a real control in range,
              never a digital DAC/ADC loopback; HDMI-in audio (the live DAC) heard, never recorded;
              juricabi.2's page: each input's headphone level and Record switch on their own, the
              output mixers never recorded, Record Level the ADC input gain, Headphone Volume the
              line out, the boosts the preamps; with nothing recorded the mic stays on the ADC at
              the minimum ADC volume (the ADC keeps a source)
  loading     clamping, fields missing from [mixer], a [mixer] of all's test build (version 1);
              nothing links two rows (Headphone Volume leaves A/V In); ripples' Audio Source and
              the receiver's audio follow the Record switches and A/V In; the first change;
              a [mixer] of juricabi.1 (Record Headphone Mix)
  settings    compatibility both ways (docs/audio.md): a setting.ini of stock ripples 1.2.2, one of
              all's firmware (082b2f5), a fresh one: the values in effect, nothing written at
              start-up, nothing written by a save without a change, all's keys kept for all; the
              round trip juricabi -> stock ripples -> juricabi (Audio Source, Live Volume)
  migration   ripples' firmware (libmedia_mpp reset + ripples' dvr.c and audio_sel.sh) vs the mixer,
              per Audio Source and Live Volume: recording, headphones, line out power and the
              receiver's audio switch must be the same (the documented difference: booting
              straight into HDZero, docs/audio.md)
  transitions a change never leaves the line out / the recording without a source in between,
              and a source recorded before and after is recorded all the way through
  worker      apply never blocks, a burst of steps coalesces, setting.ini only on save; set()
              clamps; hold: only the held batch while held, the state applied on release
  hold        ripples' DVR playback and test paths while held: no headphone source of the page heard
              with the file, a test records only its source, the AIF1 DAC timeslot volumes back
              afterwards; two holders (playback and a test) hold independently
  watcher     the codec is read only while something may have changed it: once after an apply
              (every control of the batch), in a recheck window (a record daemon start, also the
              one running at boot; audio_mixer_recheck()); otherwise nothing at all (a read clicks
              in the recording); a libmedia_mpp reset after a daemon start is undone; a control the
              codec does not keep, a codec that cannot be read, an amixer that fails: retried
              with a backoff, never a loop, never given up; recheck looks at once; not while held
  daemon restart  while the line out is off (stock boot state, HDMI in, Headphone Volume 0) a
              record daemon restart makes nothing audible
--src runs the same against other mixer sources (mutants.py); --driver also checks that every
modelled route is one of the driver's.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
import codec_model as cm  # noqa: E402

TARGETS = ["HDZGOGGLE", "HDZGOGGLE2", "HDZBOXPRO"]
MIXER_KEYS = ["mic", "line_in", "av", "hdmi", "rec_mic", "rec_line_in", "rec_av", "rx_audio", "phones_at_boot", "rec_level"]
RECORD_KEYS = ["live_audio_volume", "mic_gain", "linein_gain", "audio_source"]  # ripples' [record] keys
KEYS = MIXER_KEYS + RECORD_KEYS
MAX = {"mic": 8, "line_in": 8, "av": 8, "hdmi": 8, "rec_mic": 1, "rec_line_in": 1, "rec_av": 1, "rx_audio": 1,
       "phones_at_boot": 1, "rec_level": 7, "live_audio_volume": 10, "mic_gain": 7, "linein_gain": 7, "audio_source": 2}
# all's [mixer] keys that juricabi's firmware does not use (ripples' controls instead): kept in setting.ini
ALL_ONLY_KEYS = ["headphones", "playback", "mic_boost", "line_in_boost", "dvr_level"]
VERSION = "2"
# the Record source switch of each analog source, and its headphone level
DIRECT = {"mic": "rec_mic", "line_in": "rec_line_in", "av": "rec_av"}
# DAC -> ADC and ADC -> DAC digital loopbacks: the mixer must never switch them on
LOOPBACKS = ["AIF1 AD0L Mixer AIF1 DA0L Switch", "AIF1 AD0R Mixer AIF1 DA0R Switch",
             "DACL Mixer ADCL Switch", "DACR Mixer ADCR Switch"]
DUMP = os.path.join(ROOT, "mkapp/app/app/record/audio/audiocodec/alsa-util/amixer-record.txt")
AUDIO_SEL = os.path.join(ROOT, "mkapp/app/script/audio_sel.sh")
WATCH_MS = 50  # the harness's watcher period (firmware: 250)
RECHECK_WINDOW = WATCH_MS * 24  # a recheck window (AUDIO_MIXER_RECHECK_WINDOW)
# output mixer gain of each analog source, and its switches
OUTPUT_GAIN = {"mic": ("MIC1_G boost stage output mixer control", "Left Output Mixer MIC1Booststage Switch"),
               "line_in": ("MIC2 BST stage to L_R outp mixer gain", "Left Output Mixer MIC2Booststage Switch"),
               "av": ("LINEINL/R to L_R output mixer gain", "Left Output Mixer LINEINL Switch")}
START = ["load", "init"]  # main.c: settings_load(), audio_mixer_init()
# keys that keep ripples' settings_load() from writing setting.ini itself (the OSD visibility at
# start-up "last", the gyro calibration migration done)
QUIET = "[osd]\nstartup_visibility=2\n[ht]\ngyr_cal_range_idx=1\n"

CONTROLS = cm.parse_amixer_contents(DUMP)
SEL = cm.AudioSel(AUDIO_SEL)
failures = []
verbose = False


def fail(test, message):
    failures.append(f"{test}: {message}")
    print(f"FAIL {test}: {message}")


def ok(test):
    if verbose:
        print(f"ok   {test}")


# ---------------------------------------------------------------------------------------------
# ripples' curves (dvr.c / audio_sel.sh of ripples 1.2.2), independent of audio_mixer_routing.c

def live_line_out(v):
    v = max(0, min(10, v))
    return (v * 31 + 5) // 10


def live_linein_gain(v):  # LINEIN output mixer gain register
    v = max(0, min(10, v))
    g = (v * v * v * 7 + 500) // 1000
    return 1 if v > 0 and g == 0 else g


# ---------------------------------------------------------------------------------------------
# harness

class Harness:
    def __init__(self, target, src, work):
        self.target = target
        self.dir = os.path.join(work, target)
        os.makedirs(self.dir, exist_ok=True)
        self.binary = os.path.join(self.dir, "harness")
        minini = os.path.join(work, "minIni.o")
        if not os.path.exists(minini):
            subprocess.run(["cc", "-std=gnu99", "-w", "-c", os.path.join(ROOT, "lib/minIni/src/minIni.c"),
                            "-I", os.path.join(ROOT, "lib/minIni/src"), "-o", minini], check=True)
        # ripples' settings.c as for the emulator (setting.ini in the current directory), upstream code: no -Wall
        settings = os.path.join(self.dir, "settings.o")
        subprocess.run(["cc", "-std=gnu99", "-w", "-O1", "-DEMULATOR_BUILD", f"-D{target}=1", "-D_GNU_SOURCE",
                        "-I", os.path.join(ROOT, "src"), "-I", os.path.join(ROOT, "src/core"),
                        "-I", os.path.join(ROOT, "lib/log/include"), "-I", os.path.join(ROOT, "lib/lvgl"),
                        "-I", os.path.join(ROOT, "lib/minIni/src"),
                        "-c", os.path.join(ROOT, "src/core/settings.c"), "-o", settings], check=True)
        cmd = ["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-pthread", f"-D{target}=1",
               f"-DAUDIO_MIXER_WATCH_MS={WATCH_MS}", "-DAUDIO_MIXER_TEST_HOOKS",
               "-I", os.path.join(HERE, "harness/include"), "-I", src, "-I", os.path.join(ROOT, "src"),
               "-I", os.path.join(ROOT, "lib/log/include"), "-I", os.path.join(ROOT, "lib/minIni/src"),
               os.path.join(HERE, "harness/harness.c"),
               os.path.join(src, "core/audio_mixer.c"), os.path.join(src, "core/audio_mixer_routing.c"),
               settings, minini, "-o", self.binary]
        subprocess.run(cmd, check=True)
        self.count = 0

    def run(self, ini, lines, timeout=30):
        """Run a script; returns (batches, queries, codec, log, ini text after)."""
        self.count += 1
        case = os.path.join(self.dir, f"case{self.count}")
        os.makedirs(case, exist_ok=True)
        ini_path = os.path.join(case, "setting.ini")
        with open(ini_path, "w") as f:
            f.write(ini)
        script = "\n".join([f"ini {ini_path}"] + lines) + "\n"
        p = subprocess.run([self.binary], input=script, cwd=case, capture_output=True, text=True, timeout=timeout)
        if p.returncode != 0:
            raise RuntimeError(f"harness failed ({p.returncode}): {p.stderr[-2000:]}")
        batches, queries, codec = [], [], {}
        self.dumps, self.marks = [], []  # every dump; number of batches at each "mark"
        it = iter(p.stdout.splitlines())
        for line in it:
            if line.startswith("BATCH "):
                text = []
                for b in it:
                    if b == "END":
                        break
                    text.append(b)
                batches.append(cm.parse_batch("\n".join(text)))
            elif line.startswith("CODEC "):
                if not codec:
                    self.dumps.append(codec)
                name, value = line[6:].rsplit("=", 1)
                codec[name] = int(value)
            elif line == "MARK":
                self.marks.append(len(batches))
                codec = {}
            else:
                if codec:
                    codec = {}
                queries.append(line)
        codec = self.dumps[-1] if self.dumps else {}
        return batches, queries, codec, p.stderr, open(ini_path).read()


def base_codec(target, libmedia=True):
    state = cm.driver_defaults(CONTROLS, target)
    if libmedia:  # the record daemon started by rc.sh has initialised
        state = cm.apply(state, cm.LIBMEDIA_RESET)
    return state


def codec_lines(state):
    return [f"codec {k}={v}" for k, v in sorted(state.items())]


def mixer_ini(values, extra_record=None):
    """A setting.ini of juricabi's firmware: ripples' keys in [record], the rest in [mixer] (version 2)."""
    record = {k: v for k, v in values.items() if k in RECORD_KEYS}
    record.update(extra_record or {})
    mixer = {k: v for k, v in values.items() if k not in RECORD_KEYS}
    text = "[record]\n" + "".join(f"{k}={v}\n" for k, v in record.items()) if record else ""
    return QUIET + text + "[mixer]\n" + "".join(f"{k}={v}\n" for k, v in mixer.items()) + f"version={VERSION}\n"


def section(text, name):
    """{key: value} of one section of a setting.ini text."""
    m = re.search(r"^\[%s\]\n(.*?)(?=^\[|\Z)" % re.escape(name), text, re.M | re.S)
    return dict(re.findall(r"^(\w+)=(-?\d+)$", m.group(1), re.M)) if m else {}


def query(queries, prefix):
    return [q for q in queries if q.startswith(prefix)]


def gets(queries):
    return {q.split()[1]: int(q.split()[2]) for q in query(queries, "GET ")}


def stat_unchanged(queries):
    stats = query(queries, "STAT ")
    return len(stats) >= 2 and len(set(stats)) == 1


# ---------------------------------------------------------------------------------------------
# the specification (docs/audio.md), independent of audio_mixer_routing.c

def ripples_live(raw):
    """[record] live_audio_volume as ripples' settings_load() reads it (values above 10 are of an
    older ripples scale: (v - 25) * 10 / 6, C division), clamped to 0..10."""
    v = int(raw)
    if v > 10:
        v = int((v - 25) * 10 / 6)
    return max(0, min(10, v))


def clamp_values(values):
    """The state in effect for a setting.ini of these values: [mixer] fields missing from it come
    from ripples' state (the A/V In level from Live Volume)."""
    live = ripples_live(values.get("live_audio_volume", 10))
    v = migration_spec(0, live=live)
    for k, x in values.items():
        if k in MAX:
            v[k] = max(0, min(MAX[k], int(x)))
    v["live_audio_volume"] = live
    return v


def expected(v, line_out=True, live_dac=False):
    """What each sink must get: {sink: {source: {(gains, output mixer side)}}}."""
    mix = {}  # sources in the output mixers: gains up to and including the output mixer
    if live_dac and v["hdmi"]:
        # live digital source (HDMI-in audio): level 1..8 = -4.5..+6 dB around playback's 152
        mix["dac"] = {("DAC volume", 152 + 2 * (v["hdmi"] - 4))}
    if v["mic"]:
        mix["mic"] = {("MIC1 boost AMP gain control", v["mic_gain"]), ("MIC1_G boost stage output mixer control", v["mic"] - 1)}
    if v["line_in"]:
        mix["line_in"] = {("MIC2 boost AMP gain control", v["linein_gain"]), ("MIC2 BST stage to L_R outp mixer gain", v["line_in"] - 1)}
    if v["av"]:
        mix["av"] = {("LINEINL/R to L_R output mixer gain", v["av"] - 1)}

    volume = live_line_out(v["live_audio_volume"])
    phones = {}
    if (line_out or live_dac) and volume > 0:
        # mono line out: both outputs carry left + right output mixer
        for source, gains in mix.items():
            g = frozenset(gains | {("lineout volume", volume)})
            phones[source] = {(g, "L"), (g, "R")}

    direct_on = {s: bool(v[k]) for s, k in DIRECT.items()}
    # nothing to record: the mic stays on the ADC (the ADC keeps a source), at the minimum ADC volume
    silent = not any(direct_on.values())
    if silent:
        direct_on["mic"] = True
    # Record Level is the ADC input gain
    adc = {("ADC input gain control", v["rec_level"]), ("ADC volume", 0 if silent else 160)}
    record_l, record_r = {}, {}
    # Record Mic / Line In / A/V In: straight into both ADCs, only the preamp in front; the output
    # mixers (the headphones) are never recorded
    direct = {"mic": {("MIC1 boost AMP gain control", v["mic_gain"])},
              "line_in": {("MIC2 boost AMP gain control", v["linein_gain"])},
              "av": set()}
    for source in DIRECT:
        if direct_on[source]:
            record_l[source] = {(frozenset(direct[source] | adc), None)}
            record_r[source] = {(frozenset(direct[source] | adc), None)}
    return {"record_l": record_l, "record_r": record_r, "phones_l": phones, "phones_r": dict(phones)}


def migration_spec(audio_source, live=10, mic_gain=4, linein_gain=3):
    """The mixer's values for ripples' [record] state (no [mixer] of version 2)."""
    sel = min(audio_source & 0xFF, 2)
    return {"mic": 0, "line_in": 0, "av": live_linein_gain(live) + 1, "hdmi": 4,
            "rec_mic": int(sel == 0), "rec_line_in": int(sel == 1), "rec_av": int(sel == 2),
            "rx_audio": int(audio_source == 2), "phones_at_boot": 0,
            # ripples' ADC input gain: Line/AV Gain with Line In / A/V In, the record daemon's 0 dB with the mic
            "rec_level": 3 if sel == 0 else linein_gain,
            "live_audio_volume": max(0, min(10, live)), "mic_gain": mic_gain, "linein_gain": linein_gain,
            "audio_source": sel}


def compare(test, got, want):
    if got == want:
        ok(test)
        return True
    fail(test, "\n got:\n" + cm.describe(got) + "\n want:\n" + cm.describe(want))
    return False


def check_batches(test, batches):
    for i, batch in enumerate(batches):
        errors = cm.check_batch(CONTROLS, batch)
        errors += [f"digital loopback '{n}' switched on" for n, value in batch if n in LOOPBACKS and value]
        if errors:
            fail(f"{test} batch {i + 1}", "; ".join(errors))
            return False
    return True


# ---------------------------------------------------------------------------------------------
# tests

def direct_only(**values):  # the Record switches as given, the others off
    return dict({"rec_mic": 0, "rec_line_in": 0, "rec_av": 0}, **values)


ROUTING_STATES = [
    ("mic recorded, a/v heard (ripples' Mic)", direct_only(rec_mic=1, av=8, mic_gain=5, live_audio_volume=10, linein_gain=6), {}),
    ("line in recorded (ripples' Line In)", direct_only(rec_line_in=1, av=8, linein_gain=5, rec_level=5, live_audio_volume=7), {}),
    ("a/v recorded (ripples' A/V In)", direct_only(rec_av=1, av=5, rec_level=1, live_audio_volume=4), {}),
    ("line in + a/v recorded, mic heard", direct_only(rec_line_in=1, rec_av=1, mic=2, linein_gain=6, rec_level=6, live_audio_volume=4), {}),
    ("mic + line in recorded, own Record Level", direct_only(rec_mic=1, rec_line_in=1, mic_gain=2, linein_gain=6, rec_level=2), {}),
    ("all three recorded and heard", direct_only(rec_mic=1, rec_line_in=1, rec_av=1, mic=8, line_in=8, av=8, mic_gain=7,
                                                linein_gain=7, rec_level=7, live_audio_volume=10), {}),
    ("recorded, not heard", direct_only(rec_mic=1, rec_av=1, mic=0, av=0, live_audio_volume=10), {}),
    ("nothing recorded: the mic on the ADC, silent", direct_only(av=8, mic=3, live_audio_volume=10, linein_gain=5, rec_level=4), {}),
    ("all off", direct_only(live_audio_volume=0, av=0), {}),
    ("hdmi in (line out off), line in recorded", direct_only(rec_line_in=1, av=8, live_audio_volume=10), {"line_out": 0}),
    ("hdmi in (line out off), nothing recorded", direct_only(mic=4, av=8, live_audio_volume=10), {"line_out": 0}),
    ("out of range in setting.ini", {"mic": 99, "line_in": -3, "av": 12, "mic_gain": 8, "linein_gain": 100,
                                     "live_audio_volume": 200, "rec_mic": 7, "rec_line_in": 2, "rec_level": 12,
                                     "rec_av": -1, "rx_audio": 3, "hdmi": 11}, {}),
    # HDMI In Audio on: the bridge plays HDMI-in audio into the DAC (live_dac); mixed with the mic, line out
    # on despite HDMI in, never recorded
    ("hdmi in audio, mic recorded and heard", direct_only(rec_mic=1, mic=4, hdmi=6, live_audio_volume=8), {"line_out": 0, "live_dac": 1}),
    ("hdmi in audio, nothing recorded", direct_only(hdmi=4, live_audio_volume=10), {"line_out": 0, "live_dac": 1}),
    ("hdmi in audio at level 0, hdmi in", direct_only(rec_mic=1, mic=4, hdmi=0, live_audio_volume=10), {"line_out": 0, "live_dac": 1}),
    ("hdmi in audio off (stock): line out off", direct_only(rec_av=1, mic=4, hdmi=8, av=8, live_audio_volume=10), {"line_out": 0, "live_dac": 0}),
]

# Every control the mixer owns, at a value it never writes: a batch must set each of them.
SCRAMBLED = dict({"ADC volume": 100, "DAC volume": 100, "lineout volume": 5, "MIC1 boost AMP gain control": 0,
                  "MIC2 boost AMP gain control": 0, "ADC input gain control": 0, "LINEOUTL Mux": 0, "LINEOUTR Mux": 0,
                  "MIC1_G boost stage output mixer control": 7, "MIC2 BST stage to L_R outp mixer gain": 7,
                  "LINEINL/R to L_R output mixer gain": 7},
                 **{f"{side} Output Mixer {s} Switch": 1 for side in ("Left", "Right")
                    for s in ("MIC1Booststage", "MIC2Booststage", "DACL", "DACR")},
                 **{"Left Output Mixer LINEINL Switch": 1, "Right Output Mixer LINEINR Switch": 1,
                    "DACL Mixer AIF1DA0L Switch": 0, "DACR Mixer AIF1DA0R Switch": 0,
                    "AIF1 AD0L Mixer ADCL Switch": 0, "AIF1 AD0R Mixer ADCR Switch": 0},
                 **{f"{lr}ADC input Mixer {s} Switch": 1 for lr in ("L", "R")
                    for s in ("MIC1 boost", "MIC2 boost", "l_output mixer", "r_output mixer")},
                 **{"LADC input Mixer LINEINL Switch": 1, "RADC input Mixer LINEINR Switch": 1})


def check_unused_gains(test, codec):
    """The output mixer gain of a source that is not connected is back at 0 dB (3)."""
    for source, (gain, switch) in OUTPUT_GAIN.items():
        if codec.get(switch) == 0 and codec.get(gain) != 3:
            fail(test, f"{source} not in the output mixers, but '{gain}' = {codec.get(gain)} (not 0 dB)")
            return False
    return True


def test_routing(h):
    for i, (name, values, ctx) in enumerate(ROUTING_STATES):
        test = f"{h.target} routing '{name}'"
        start = base_codec(h.target)
        if i % 2:  # every other state from a codec with every owned control at a wrong value
            start = dict(start, **SCRAMBLED)
            test += " (scrambled codec)"
        lines = codec_lines(start) + START + ["apply"]
        # (after a switch to an analog source unless the state says otherwise)
        lines.append(f"line_out {ctx.get('line_out', 1)}")
        if "live_dac" in ctx:
            lines.append(f"live_dac {ctx['live_dac']}")
        lines += ["wait", "dump"]
        batches, _, codec, _, _ = h.run(mixer_ini(values), lines)
        if not batches:
            fail(test, "no batch written")
            continue
        check_batches(test, batches)
        live_dac = ctx.get("live_dac", 0) == 1
        want = expected(clamp_values(values), line_out=ctx.get("line_out", 1) == 1, live_dac=live_dac)
        if compare(test, cm.evaluate(codec, dac_active=live_dac), want):
            check_unused_gains(test, codec)


def test_loading(h):
    # clamping on load ([mixer] and ripples' keys)
    values = dict(next(v for name, v, _ in ROUTING_STATES if name == "out of range in setting.ini"))
    _, queries, _, _, _ = h.run(mixer_ini(values), START + [f"get {k}" for k in KEYS] + ["rx"])
    got = gets(queries)
    want = clamp_values(values)
    want["audio_source"] = 0  # (rec_mic the one direct source)
    if got != want:
        fail(f"{h.target} load clamps", f"got {got}, want {want}")
    else:
        ok(f"{h.target} load clamps")

    # a field missing from [mixer] comes from ripples' state
    _, queries, _, _, _ = h.run(QUIET + f"[record]\naudio_source=2\nlive_audio_volume=7\n[mixer]\nversion={VERSION}\nmic=5\n",
                                START + [f"get {k}" for k in KEYS])
    got = gets(queries)
    want = dict(migration_spec(2, live=7), mic=5)
    if got != want:
        fail(f"{h.target} load missing fields", f"got {got}, want {want}")
    else:
        ok(f"{h.target} load missing fields")

    # a [mixer] of all's unreleased version 1, or one cut short by a power loss before its version
    # was written: ripples' state, the section removed (one write); the next change writes a clean one
    for audio_source in (0, 1, 2):
        for name, section_text in (
                ("version 1", "mic=4\nline_in=3\nav=2\nheadphones=12\ndvr_mode=0\ndvr_mic=1\n"
                              "dvr_line_in=1\ndvr_av=1\ndvr_level=6\nhdmi=7\nversion=1\n"),
                ("cut short (no version)", "mic=7\nrec_mix=1\n")):
            test = f"{h.target} load {name} [mixer], audio_source={audio_source}"
            old = QUIET + "[record]\naudio_source=%d\n[mixer]\n%s" % (audio_source, section_text)
            _, queries, _, _, after = h.run(old, START + [f"get {k}" for k in KEYS] + ["set hdmi 5", "save"])
            got = gets(queries)
            want = migration_spec(audio_source)
            written = section(after, "mixer")
            if got != want:
                fail(test, f"got {got}, want {want}")
            elif written != {"hdmi": "5", "version": VERSION, "phones_at_boot": "1",
                             "record_source": str(audio_source), "record_live": "10"}:
                fail(test, f"[mixer] after a change written as {written}")
            else:
                ok(test)

    # nothing on the page links two rows; ripples' Audio Source (no row: the first recorded input, for
    # ripples' firmware) and the receiver's audio (A/V In heard or recorded) follow
    test = f"{h.target} Audio Source and receiver audio follow the Record switches and A/V In"
    lines = START + ["set rec_mic 0", "set rec_av 1", "get audio_source", "rx", "set av 0", "rx", "set rec_av 0", "rx",
                     "get audio_source", "set rec_line_in 1", "get audio_source", "set rec_mic 1", "get audio_source",
                     "get mic", "get av", "get live_audio_volume"]
    _, queries, _, _, _ = h.run(QUIET + "[record]\naudio_source=0\n", lines)
    sources = [int(q.split()[2]) for q in query(queries, "GET audio_source")]
    rx = [int(q.split()[1]) for q in query(queries, "RX ")]
    others = {q.split()[1]: int(q.split()[2]) for q in query(queries, "GET ") if q.split()[1] != "audio_source"}
    if sources != [2, 2, 1, 1] or rx != [1, 1, 0] or others != {"mic": 0, "av": 0, "live_audio_volume": 10}:
        fail(test, f"audio_source {sources} (want [2, 2, 1, 1]), receiver audio {rx} (want [1, 1, 0]), {others}")
    else:
        ok(test)
    test = f"{h.target} Headphone Volume leaves the A/V In level and the headphones at boot"
    bad = []
    for live in range(0, 11):
        _, queries, _, _, _ = h.run("", START + [f"set_live {live}", "get av", "get live_audio_volume", "get phones_at_boot"])
        got = gets(queries)
        if got != {"av": live_linein_gain(10) + 1, "live_audio_volume": live, "phones_at_boot": 0}:
            bad.append(f"{live}: {got}")
    if bad:
        fail(test, "; ".join(bad))
    else:
        ok(test)

    # the first change: only the changed fields ([mixer] ones and the [record] values they belong to,
    # then the version; ripples' in [record])
    test = f"{h.target} first change on the Audio page"
    ini = QUIET + "[record]\naudio_source=1\n"
    _, _, _, _, after = h.run(ini, START + ["set mic 3", "set mic_gain 6", "save", "save"])
    lines = [l for l in after.split("[mixer]", 1)[-1].splitlines() if l.strip()] if "[mixer]" in after else []
    record = sorted(section(after, "record").items())
    _, queries, _, _, _ = h.run(after, START + [f"get {k}" for k in KEYS])
    got = gets(queries)
    want = dict(migration_spec(1, mic_gain=6), mic=3, phones_at_boot=1)
    if sorted(lines[:-1]) != ["mic=3", "phones_at_boot=1", "record_live=10", "record_source=1"] or lines[-1:] != [f"version={VERSION}"]:
        fail(test, f"[mixer] written as {lines}")
    elif record != [("audio_source", "1"), ("mic_gain", "6")]:
        fail(test, f"[record] written as {record}")
    elif got != want:
        fail(test, f"read back as {got}, want {want}")
    else:
        ok(test)


# a setting.ini of stock ripples 1.2.2 (as its page writes it; dvr_audio_volume_v2 since its migration;
# Record Audio as settings_get_bool() stores it, "true" / "false")
def ripples_ini(audio_source, live, mic_gain, linein_gain, dvr=8, audio=True):
    return ("[record]\naudio=%s\naudio_source=%d\ndvr_audio_volume_v2=%d\nlive_audio_volume=%d\nmic_gain=%d\n"
            "linein_gain=%d\n[osd]\nstartup_visibility=2\n[ht]\ngyr_cal_range_idx=1\n"
            % ("true" if audio else "false", audio_source, dvr, live, mic_gain, linein_gain))


# a setting.ini of all's firmware 082b2f5 after its Audio page was used: [mixer] version 2 (with all's
# own headphones / playback / boosts / record level), the old [record] audio_source, [hdmi]
def all_ini(mixer, audio_source=0, record=""):
    return ("[record]\naudio=true\naudio_source=%d\n%s[osd]\nstartup_visibility=2\n[ht]\ngyr_cal_range_idx=1\n"
            "[mixer]\n%sversion=2\n[hdmi]\naudio_out=0\naudio_in=0\n"
            % (audio_source, record, "".join(f"{k}={v}\n" for k, v in mixer.items())))


def test_settings(h):
    """Compatibility both ways (docs/audio.md "Settings")."""
    # stock ripples: every Audio Source, a few Live Volumes and gains: ripples' values, the mixer's state as
    # ripples' firmware set it, nothing written by the mixer at start-up or by a save without a change
    for audio_source, live, mic_gain, linein_gain, dvr in ((0, 10, 4, 3, 8), (1, 6, 2, 5, 3), (2, 3, 7, 0, 0), (2, 0, 0, 7, 5)):
        test = f"{h.target} settings: stock ripples audio_source={audio_source} live={live} gains={mic_gain}/{linein_gain}"
        ini = ripples_ini(audio_source, live, mic_gain, linein_gain, dvr)
        _, queries, _, _, after = h.run(ini, ["load", "stat", "init", "save", "stat", "record"] + [f"get {k}" for k in KEYS] + ["rx", "unsaved"])
        got = gets(queries)
        want = migration_spec(audio_source, live, mic_gain, linein_gain)
        rec = query(queries, "REC ")
        if got != want:
            fail(test, f"got {got}, want {want}")
        elif rec != [f"REC {audio_source} {live} {dvr} {mic_gain} {linein_gain}"]:
            fail(test, f"g_setting.record after init {rec}")
        elif not stat_unchanged(queries) or after != ini:
            fail(test, "setting.ini written by the mixer at start-up or by a save without a change")
        elif query(queries, "UNSAVED ") != ["UNSAVED 0"] or query(queries, "RX ") != [f"RX {int(audio_source == 2)}"]:
            fail(test, f"{query(queries, 'UNSAVED ')} {query(queries, 'RX ')}")
        else:
            ok(test)

    # a fresh setting.ini: ripples' defaults
    test = f"{h.target} settings: fresh setting.ini"
    _, queries, _, _, after = h.run("", ["load", "stat", "init", "save", "stat", "record"] + [f"get {k}" for k in KEYS])
    got = gets(queries)
    if got != migration_spec(0) or query(queries, "REC ") != ["REC 0 10 8 4 3"]:
        fail(test, f"got {got} {query(queries, 'REC ')}, want {migration_spec(0)} REC 0 10 8 4 3")
    elif not stat_unchanged(queries):
        fail(test, "setting.ini written by the mixer")
    else:
        ok(test)

    # all's firmware: its [mixer] values in effect, ripples' values from all's equivalents, the Audio Source
    # from its Record switches; nothing written; after a change on the page all's keys are still there for all
    cases = (
        ("defaults of all's migration (A/V In)",
         dict(mic=0, line_in=0, av=8, hdmi=4, rec_mix=0, rec_mic=0, rec_line_in=0, rec_av=1, rx_audio=1, phones_at_boot=0,
              headphones=31, playback=27, mic_boost=4, line_in_boost=4, dvr_level=3), 2,
         {"rec_level": 3, "live_audio_volume": 10, "mic_gain": 4, "linein_gain": 4, "audio_source": 2}, 2),
        # Record Headphone Mix (rec_mix=1): the inputs it heard (mic, a/v) are recorded directly
        ("used: mix, mic heard, line in recorded", dict(mic=5, line_in=0, av=6, hdmi=7, rec_mix=1, rec_mic=0, rec_line_in=1,
                                                      rec_av=0, rx_audio=0, phones_at_boot=1, headphones=16, playback=31,
                                                      mic_boost=6, line_in_boost=2, dvr_level=5), 0,
         {"rec_mic": 1, "rec_av": 1, "rec_level": 5, "live_audio_volume": 5, "mic_gain": 6, "linein_gain": 2,
          "audio_source": 0}, 7),
        # ripples' Audio Source: the stored one is not recorded, the first recorded input (the mic) instead
        ("several direct sources (Audio Source: the first recorded)", dict(mic=0, line_in=0, av=8, hdmi=4, rec_mix=0, rec_mic=1,
                                                                         rec_line_in=0, rec_av=1, rx_audio=1, phones_at_boot=0,
                                                                         headphones=0, playback=0, mic_boost=0, line_in_boost=7,
                                                                         dvr_level=7), 1,
         {"rec_level": 7, "live_audio_volume": 0, "mic_gain": 0, "linein_gain": 7, "audio_source": 0}, 0),
        # all's headphones 0..31 to Live Volume 0..10, rounded: 27 -> 8.7 -> 9
        ("headphones between two Live Volumes", dict(mic=0, line_in=0, av=8, hdmi=4, rec_mix=0, rec_mic=0, rec_line_in=0,
                                                    rec_av=1, rx_audio=1, phones_at_boot=0, headphones=27, playback=27,
                                                    mic_boost=4, line_in_boost=4, dvr_level=3), 2,
         {"rec_level": 3, "live_audio_volume": 9, "mic_gain": 4, "linein_gain": 4, "audio_source": 2}, 2),
        # ripples' firmware before all's: ripples' own Live Volume is kept, not all's headphones
        ("ripples' Live Volume from before all's", dict(mic=0, line_in=0, av=8, hdmi=4, rec_mix=0, rec_mic=0, rec_line_in=0,
                                                       rec_av=1, rx_audio=1, phones_at_boot=0, headphones=27, playback=27,
                                                       mic_boost=4, line_in_boost=4, dvr_level=3), 2,
         {"rec_level": 3, "live_audio_volume": 4, "mic_gain": 4, "linein_gain": 4, "audio_source": 2}, 2, "live_audio_volume=4\n"),
    )
    for name, mixer, audio_source, want_ripples, want_dvr, *record in cases:
        test = f"{h.target} settings: all's firmware, {name}"
        ini = all_ini(mixer, audio_source, record[0] if record else "")
        _, queries, _, _, after = h.run(ini, ["load", "stat", "init", "save", "stat", "record"] + [f"get {k}" for k in KEYS])
        got = gets(queries)
        want = dict({k: mixer[k] for k in MIXER_KEYS if k in mixer}, **want_ripples)
        rec = query(queries, "REC ")
        want_rec = (f"REC {want_ripples['audio_source']} {want_ripples['live_audio_volume']} {want_dvr} "
                    f"{want_ripples['mic_gain']} {want_ripples['linein_gain']}")
        if got != want:
            fail(test, f"got {got}, want {want}")
            continue
        if rec != [want_rec]:
            fail(test, f"g_setting.record {rec}, want {want_rec}")
            continue
        if not stat_unchanged(queries) or after != ini:
            fail(test, "setting.ini written at start-up")
            continue
        # a change on the page: all's own keys stay (all reads the same values back), ripples' key written
        _, queries, _, _, after = h.run(ini, START + ["set_live 8", "set hdmi 2", "save"])
        kept = section(after, "mixer")
        lost = [k for k in ALL_ONLY_KEYS if kept.get(k) != str(mixer[k])]
        # ... and the next start shows what the page showed (the Audio Source as well)
        _, queries, _, _, _ = h.run(after, START + [f"get {k}" for k in KEYS])
        again = gets(queries)
        # (a headphone level changed: the headphones from boot on; Headphone Volume leaves A/V In)
        want_again = dict(want, live_audio_volume=8, hdmi=2, phones_at_boot=1)
        file_source = section(after, "record").get("audio_source")
        if lost or kept.get("version") != VERSION or "live_audio_volume=8" not in after or kept.get("hdmi") != "2" \
                or "rec_mix" in kept:
            fail(test, f"after a change: all's keys changed or lost {lost}, [mixer] {kept}")
        elif file_source != str(want_ripples["audio_source"]):
            # (stock ripples, flashed back, reads this key: the Audio Source the page showed)
            fail(test, f"after a change: [record] audio_source={file_source} in the file, "
                       f"want {want_ripples['audio_source']} as the page shows")
        elif again != want_again:
            fail(test, f"after a change and a restart: {again}, want {want_again}")
        else:
            ok(test)

    # back to stock ripples after juricabi's firmware changed its rows: ripples reads its own keys as written
    test = f"{h.target} settings: back to stock ripples"
    _, _, _, _, after = h.run(ripples_ini(0, 10, 4, 3), START + ["set rec_mic 0", "set rec_av 1", "set_live 6", "set mic_gain 2",
                                                                "set linein_gain 6", "save"])
    _, queries, _, _, _ = h.run(after, ["load", "record"])  # stock ripples: settings.c only
    if query(queries, "REC ") != ["REC 2 6 8 2 6"]:
        fail(test, f"stock ripples would read {query(queries, 'REC ')}, want REC 2 6 8 2 6")
    else:
        ok(test)

    test_round_trip(h)
    test_juricabi1(h)


def test_juricabi1(h):
    """A [mixer] of juricabi.1 with Record Headphone Mix (rec_mix=1): each input it heard is recorded directly,
    the Record Level is what the ADC had (Line/AV Gain, more than the mic recorded); nothing written at the start
    or by a save without a change; the next change writes the Record switches and Record Level and removes
    rec_mix; a [mixer] of juricabi.1 without the mix keeps its Record switches, Record Level from its rule."""
    test = f"{h.target} settings: juricabi.1, Record Headphone Mix"
    ini = (QUIET + "[record]\naudio_source=0\nlinein_gain=5\n[mixer]\nmic=4\nline_in=0\nav=6\nrec_mix=1\nrec_mic=0\n"
           "rec_line_in=0\nrec_av=0\nrecord_source=0\nrecord_live=10\nversion=2\n")
    _, queries, _, _, after = h.run(ini, ["load", "stat", "init", "save", "stat"] + [f"get {k}" for k in KEYS])
    got = gets(queries)
    want = {"rec_mic": 1, "rec_line_in": 0, "rec_av": 1, "rec_level": 5, "mic": 4, "av": 6, "audio_source": 0}
    bad = {k: (got[k], v) for k, v in want.items() if got[k] != v}
    if bad:
        fail(test, f"in effect (got, want): {bad}")
    elif not stat_unchanged(queries) or after != ini:
        fail(test, "setting.ini written at the start or by a save without a change")
    else:
        _, _, _, _, saved = h.run(ini, START + ["set hdmi 5", "save"])
        mixer = section(saved, "mixer")
        _, queries, _, _, _ = h.run(saved, START + [f"get {k}" for k in KEYS])
        again = gets(queries)
        if "rec_mix" in mixer or mixer.get("rec_mic") != "1" or mixer.get("rec_av") != "1" or mixer.get("rec_level") != "5":
            fail(test, f"after the next change [mixer] is {mixer}")
        elif any(again[k] != v for k, v in want.items()):
            fail(test, f"after the next change and a restart: {again}")
        else:
            ok(test)
    test = f"{h.target} settings: juricabi.1, Record switches without the mix"
    for rec, want_level in (("rec_mic=1\nrec_line_in=0\nrec_av=0\n", 3), ("rec_mic=0\nrec_line_in=0\nrec_av=1\n", 5)):
        ini = (QUIET + "[record]\naudio_source=0\nlinein_gain=5\n[mixer]\nmic=4\nrec_mix=0\n" + rec +
               "record_source=0\nrecord_live=10\nversion=2\n")
        _, queries, _, _, _ = h.run(ini, START + ["get rec_level", "get rec_mic", "get rec_av"])
        got = gets(queries)
        if got["rec_level"] != want_level:
            fail(test, f"{rec!r}: Record Level {got['rec_level']}, want {want_level}")
            break
    else:
        ok(test)


def ripples_putl(text, key, value):
    """stock ripples' ini_putl("record", key, value): the value replaced in place, or the key added"""
    if re.search(r"(?m)^%s=" % re.escape(key), section_text(text, "record")):
        head, rec, tail = split_section(text, "record")
        return head + re.sub(r"(?m)^(%s=).*$" % re.escape(key), r"\g<1>%d" % value, rec) + tail
    if "[record]\n" in text:
        return text.replace("[record]\n", f"[record]\n{key}={value}\n", 1)
    return text + f"[record]\n{key}={value}\n"


def split_section(text, name):
    m = re.search(r"(?ms)^\[%s\]\n.*?(?=^\[|\Z)" % re.escape(name), text)
    return (text[:m.start()], text[m.start():m.end()], text[m.end():]) if m else (text, "", "")


def section_text(text, name):
    return split_section(text, name)[1]


ROUND_KEYS = ["rec_mic", "rec_line_in", "rec_av", "rx_audio", "rec_level", "av", "audio_source", "live_audio_volume"]


def test_round_trip(h):
    """juricabi -> stock ripples -> juricabi (review of phase 3, finding 2): what stock ripples
    changed in [record] wins over the [mixer] values derived from the earlier [record] values; nothing
    is written at the start; the next change writes them; what ripples did not change stays."""
    cases = (
        # name, juricabi steps, the [record] edit of stock ripples, values in effect afterwards
        ("Record Line In only, then ripples Mic", ["set rec_mic 0", "set rec_line_in 1"], ("audio_source", 0),
         dict(rec_mic=1, rec_line_in=0, rec_av=0, rx_audio=0, rec_level=3, audio_source=0)),
        ("Record Line In only, then ripples A/V In", ["set rec_mic 0", "set rec_line_in 1"], ("audio_source", 2),
         dict(rec_mic=0, rec_line_in=0, rec_av=1, rx_audio=1, rec_level=3, audio_source=2)),
        ("Headphone Volume 3, then ripples 10", ["set_live 3"], ("live_audio_volume", 10),
         dict(av=live_linein_gain(10) + 1, live_audio_volume=10)),
        ("Headphone Volume 3 and a trimmed A/V In, then ripples 7", ["set_live 3", "set av 6"], ("live_audio_volume", 7),
         dict(av=live_linein_gain(7) + 1, live_audio_volume=7)),
        # not changed by ripples: the page's own Record switches stay, and so does ripples' Audio Source
        ("own Record switches (Line In + A/V In), ripples wrote the same Audio Source",
         ["set rec_mic 0", "set rec_line_in 1", "set rec_av 1"], ("audio_source", 1),
         dict(rec_mic=0, rec_line_in=1, rec_av=1, rx_audio=1, audio_source=1)),
        ("own Record switches and a level, ripples wrote the same Audio Source", ["set rec_av 1", "set av 5"],
         ("audio_source", 0), dict(rec_mic=1, rec_line_in=0, rec_av=1, rx_audio=1, av=5, audio_source=0)),
    )
    for name, steps, (key, value), want in cases:
        test = f"{h.target} settings: round trip, {name}"
        _, _, _, _, ini = h.run(ripples_ini(0, 10, 4, 3), START + steps + ["save"])
        ini = ripples_putl(ini, key, value)  # stock ripples
        _, queries, _, _, after = h.run(ini, ["load", "stat", "init", "stat", "save", "stat", "record"] +
                                        [f"get {k}" for k in ROUND_KEYS])
        got = gets(queries)
        bad = {k: (got[k], v) for k, v in want.items() if got[k] != v}
        rec = query(queries, "REC ")[0].split()
        if bad:
            fail(test, f"in effect (got, want): {bad}")
        elif not stat_unchanged(queries) or after != ini:
            fail(test, "setting.ini written at the start or by a save without a change")
        elif (int(rec[1]), int(rec[2])) != (got["audio_source"], got["live_audio_volume"]):
            fail(test, f"g_setting.record {rec} differs from the page's values")
        else:
            # the next change writes the values in effect: a third start reads the same (and so does ripples)
            _, _, _, _, saved = h.run(after, START + ["set hdmi 6", "save"])
            _, queries, _, _, _ = h.run(saved, START + [f"get {k}" for k in ROUND_KEYS])
            again = gets(queries)
            if any(again[k] != v for k, v in want.items()):
                fail(test, f"after the next change and a restart: {again}, want {want}")
            elif section(saved, "record").get(key) != str(value):
                fail(test, f"[record] {key} changed by juricabi's firmware: {section(saved, 'record')}")
            else:
                ok(test)


def old_firmware(target, audio_source, live, mic_gain, linein_gain, flow):
    """Codec state and receiver audio of stock ripples (record daemon up, then the app): dvr.c of 1.2.2."""
    state = base_codec(target)
    sel = min(audio_source & 0xFF, 2)

    def select():  # dvr_select_audio_values(): audio_sel.sh in_*, then the gain of that source
        cmds = SEL(["in_mic1", "in_mic2", "in_linein"][sel])
        if sel == 0:
            cmds += [("MIC1 boost AMP gain control", mic_gain)]
        else:
            cmds += [("ADC input gain control", linein_gain), ("MIC2 boost AMP gain control", linein_gain)]
        return cmds

    if flow in ("switch", "hdmi"):
        # page_source / app_switch_to_analog: dvr_select_audio_source + dvr_enable_line_out(true)
        state = cm.apply(state, select() + SEL("out_on") + [("lineout volume", live_line_out(live)),
                                                             ("LINEINL/R to L_R output mixer gain", live_linein_gain(live))]
                         + SEL("out_linein_on") + SEL("out_dac_off"))
    if flow == "hdmi":
        state = cm.apply(state, SEL("out_off"))  # app_switch_to_hdmi_in: dvr_enable_line_out(false)
    state = cm.apply(state, select())  # recording start: dvr_update_record_conf
    rx = int(audio_source == 2)  # rtc6715.init(1, audio_source == SETTING_RECORD_AUDIO_SOURCE_AV_IN)
    return state, rx


def new_firmware(h, ini, flow):
    lines = START + ["apply"]  # main.c
    if flow == "late_reset":
        lines = codec_lines(base_codec(h.target, libmedia=False)) + lines + ["wait", "wait", "line_out 1", "wait"]
        # the record daemon initialises after the app has routed (boot race, SD check, format): its
        # pid file, then its reset
        lines += [f"sleep {WATCH_MS * 3}", "recorder 2"] + codec_lines(dict(cm.LIBMEDIA_RESET)) + [f"sleep {WATCH_MS * 8}"]
    else:
        lines = codec_lines(base_codec(h.target)) + lines
        if flow in ("switch", "hdmi"):
            lines += ["wait", "line_out 1"]  # dvr_select_audio_source (waits), dvr_enable_line_out(true)
        if flow == "hdmi":
            lines += ["line_out 0"]
        lines += ["wait"]  # recording start
    lines += ["wait", "rx", "dump"]
    batches, queries, codec, log, _ = h.run(ini, lines)
    rx = int(query(queries, "RX ")[0].split()[1])
    return codec, rx, batches, log


def test_migration(h):
    names = {0: "Mic", 1: "Line In", 2: "A/V In", 3: "3 (= A/V In)", -1: "-1 (= A/V In)"}
    for audio_source, live, mic_gain, linein_gain in ((0, 10, 4, 3), (1, 10, 4, 3), (2, 10, 4, 3), (3, 7, 2, 6), (-1, 4, 6, 1),
                                                      (0, 5, 7, 6), (1, 2, 0, 7), (2, 1, 3, 2)):
        for flow in ("switch", "hdmi", "boot", "late_reset"):
            test = f"{h.target} ripples' Audio Source {names[audio_source]}, Live Volume {live}, gains {mic_gain}/{linein_gain}, {flow}"
            ini = ripples_ini(audio_source, live, mic_gain, linein_gain)
            new, new_rx, batches, log = new_firmware(h, ini, flow)
            check_batches(test, batches)
            old, old_rx = old_firmware(h.target, audio_source, live, mic_gain, linein_gain,
                                       "switch" if flow == "late_reset" else flow)
            got, want = cm.evaluate(new), cm.evaluate(old)
            if new_rx != old_rx:
                fail(test, f"analog receiver audio {new_rx}, ripples {old_rx}")
                continue
            if cm.line_out_powered(new) != cm.line_out_powered(old):
                fail(test, f"line out powered {cm.line_out_powered(new)}, ripples {cm.line_out_powered(old)}")
                continue
            if flow == "late_reset" and "codec changed by someone else" not in log:
                fail(test, "the reset was not noticed")
                continue
            # booting straight into HDZero included: ripples left the record daemon's silent line out
            # (27, idle left DAC only) until the first switch to an analog source
            compare(test, got, want)


def test_phones_at_boot(h):
    """The headphones at boot: stock (silent until the first analog source) until a headphone level of a
    source is changed; from then on, from boot. Other changes (recording, gains, Live Volume) keep it."""
    values = dict(migration_spec(2))
    for name, ini_values, change, want_live, want_flag in (
            ("untouched", values, [], False, 0),
            ("a Record switch changed", values, ["set rec_line_in 1", "apply"], False, 0),
            ("Headphone Volume changed", values, ["set_live 6", "apply"], False, 0),
            ("Mic Gain changed", values, ["set mic_gain 6", "apply"], False, 0),
            ("Mic changed", values, ["set mic 3", "apply"], True, 1),
            ("A/V In changed", values, ["set av 5", "apply"], True, 1),
            ("A/V In set to its value", values, ["set av 8", "apply"], False, 0),
            ("HDMI In level changed", values, ["set hdmi 6", "apply"], True, 1),
            ("Mic changed while HDMI in is shown", values, ["line_out 0", "set mic 3", "apply"], False, 1),
            ("Mic changed while HDMI in is shown, then analog", values,
             ["line_out 0", "set mic 3", "apply", "wait", "line_out 1"], True, 1),
            ("phones_at_boot=1 in setting.ini", dict(values, phones_at_boot=1), [], True, 1)):
        test = f"{h.target} headphones at boot, {name}"
        lines = codec_lines(base_codec(h.target)) + START + ["apply", "wait"] + change + ["wait", "get phones_at_boot", "dump"]
        _, queries, codec, _, _ = h.run(mixer_ini(ini_values), lines)
        live = cm.line_out_powered(codec)
        flag = int(query(queries, "GET phones_at_boot")[0].split()[2])
        if live != want_live or flag != want_flag:
            fail(test, f"line out powered {live}, phones_at_boot {flag}; want {want_live}, {want_flag}")
        else:
            ok(test)


TRANSITIONS = [
    # name, start values, start context, change (set lines / context), dac active during the batch
    ("switch the only heard source (mic -> a/v)", direct_only(rec_mic=1, mic=4, live_audio_volume=10), [],
     ["set mic 0", "set av 8", "apply"], False),
    ("record a/v instead of the mic (a/v heard)", direct_only(rec_mic=1, av=8, live_audio_volume=10), [],
     ["set rec_mic 0", "set rec_av 1", "apply"], False),
    ("record line in instead of a/v", direct_only(rec_av=1, av=8, live_audio_volume=10), [],
     ["set rec_av 0", "set rec_line_in 1", "apply"], False),
    ("the last recorded input off: the mic stays on the ADC, silent", direct_only(rec_av=1, av=8, live_audio_volume=10), [],
     ["set rec_av 0", "apply"], False),
    ("nothing recorded -> the mic recorded", direct_only(av=8, live_audio_volume=10), [],
     ["set rec_mic 1", "apply"], False),
    ("nothing recorded -> a/v recorded", direct_only(av=8, live_audio_volume=10), [],
     ["set rec_av 1", "apply"], False),
    ("a second input recorded (mic + a/v)", direct_only(rec_mic=1, av=8, live_audio_volume=10), [],
     ["set rec_av 1", "apply"], False),
    ("Headphone Volume down", direct_only(rec_mic=1, av=8, live_audio_volume=10), [],
     ["set_live 4", "apply"], False),
    ("hdmi in: line out off", direct_only(rec_mic=1, mic=4, av=8, live_audio_volume=10), [],
     ["line_out 0"], False),
    ("hdmi in, direct a/v: line out off, a/v leaves the output mixers", direct_only(rec_av=1, av=8, live_audio_volume=10), [],
     ["line_out 0"], False),
    ("analog after hdmi in: a/v back in the output mixers, line out on", direct_only(rec_av=1, av=8, live_audio_volume=10),
     ["line_out 0"], ["line_out 1"], False),
    ("Headphone Volume 0", direct_only(rec_mic=1, av=8, live_audio_volume=6), [],
     ["set live_audio_volume 0", "apply"], False),
    ("hdmi-in audio starts (mic heard)", direct_only(rec_mic=1, mic=4, hdmi=4, live_audio_volume=10), ["line_out 0"],
     ["live_dac 1"], True),
    ("hdmi-in audio: the only heard source changes (dac -> mic)", direct_only(rec_mic=1, hdmi=4, live_audio_volume=10),
     ["line_out 0", "live_dac 1"], ["set hdmi 0", "set mic 4", "apply"], True),
    ("hdmi-in audio ends (hdmi in still shown)", direct_only(rec_mic=1, mic=4, hdmi=4, live_audio_volume=10),
     ["line_out 0", "live_dac 1"], ["live_dac 0"], True),
]


# audio_mixer_build_held_commands(): the live paths the mixer connects, out (docs/audio.md)
HELD_OFF = ["Left Output Mixer MIC1Booststage Switch", "Right Output Mixer MIC1Booststage Switch",
            "Left Output Mixer MIC2Booststage Switch", "Right Output Mixer MIC2Booststage Switch",
            "Left Output Mixer LINEINL Switch", "Right Output Mixer LINEINR Switch",
            "Left Output Mixer DACL Switch", "Right Output Mixer DACR Switch",
            "Left Output Mixer DACR Switch", "Right Output Mixer DACL Switch",
            "LADC input Mixer l_output mixer Switch", "RADC input Mixer r_output mixer Switch",
            "LADC input Mixer r_output mixer Switch", "RADC input Mixer l_output mixer Switch"]
HELD_BATCH = [(name, 0) for name in HELD_OFF]


def dvr_volume_lines(level):
    """ripples' dvr_set_dvr_audio_volume(level) (dvr.c of 1.2.2): line out, DAC and both AIF1 DAC timeslots"""
    hw = [0, 26, 28, 29, 29, 30, 30, 31, 31][level]
    dac = 0 if level == 0 else {4: 153, 6: 157, 7: 158}.get(level, (hw * 160 + 15) // 31)
    return [f"codec lineout volume={hw}", f"codec DAC volume={dac}",
            f"codec AIF1 DAC timeslot 0 volume={dac}", f"codec AIF1 DAC timeslot 1 volume={dac}"]


# dvr_mute_live_audio(): audio_sel.sh out_linein_off, LINEIN output mixer gain 0
DVR_MUTE = codec_lines(dict(SEL("out_linein_off"))) + ["codec LINEINL/R to L_R output mixer gain=0"]


def test_hold(h):
    """ripples' playback and tests while the mixer is held (review of phase 3, findings 1, 4, 5, 7)."""
    # DVR playback with Mic and Line In heard (line out on): the held batch first, then media_init()'s
    # libmedia_mpp reset and adec2ao_start(); only the file is heard
    test = f"{h.target} hold: DVR playback with the page's headphone sources"
    values = direct_only(rec_mic=1, mic=4, line_in=3, av=8, live_audio_volume=10, phones_at_boot=1)
    lines = codec_lines(base_codec(h.target)) + START + ["wait", "line_out 1", "wait", "mark", "hold 1", "mark"]
    lines += codec_lines(dict(cm.LIBMEDIA_RESET)) + DVR_MUTE + dvr_volume_lines(5) + ["dump", "hold 0", "wait", "dump"]
    batches, _, _, _, _ = h.run(mixer_ini(values), lines)
    during, after = h.dumps[0], h.dumps[-1]
    held = batches[h.marks[0]:h.marks[1]]
    want = expected(clamp_values(values))
    if held != [HELD_BATCH]:
        fail(test, f"the first hold wrote {held}, want the held batch {HELD_BATCH}")
    elif cm.heard(during, dac_active=True) != {"dac"}:
        fail(test, f"heard with the file: {sorted(cm.heard(during, dac_active=True))}, want only the DAC")
    elif cm.recorded_via_mix(during, dac_active=True):
        fail(test, f"recorded through the output mixers while held: {sorted(cm.recorded_via_mix(during, dac_active=True))}")
    elif cm.evaluate(after) != want:
        fail(test, "after release:\n" + cm.describe(cm.evaluate(after)) + "\n want:\n" + cm.describe(want))
    elif (after.get("AIF1 DAC timeslot 0 volume"), after.get("AIF1 DAC timeslot 1 volume")) != (160, 160):
        fail(test, f"AIF1 DAC timeslot volumes after release {after.get('AIF1 DAC timeslot 0 volume')}, "
                   f"{after.get('AIF1 DAC timeslot 1 volume')}, want 160, 160 (HDMI-in audio plays through timeslot 0)")
    else:
        ok(test)

    # the Audio page's Mic test with Line In and A/V In recorded and heard: ripples' routing (in_mic1, its
    # gain, dvr_mute_live_audio) records the mic directly and nothing else
    test = f"{h.target} hold: the Mic test records only the mic"
    values = direct_only(rec_line_in=1, rec_av=1, mic=4, line_in=3, av=8, live_audio_volume=10)
    lines = codec_lines(base_codec(h.target)) + START + ["wait", "line_out 1", "wait", "hold_test 1"]
    lines += codec_lines(dict(SEL("in_mic1"))) + ["codec MIC1 boost AMP gain control=4"] + DVR_MUTE + ["dump", "hold_test 0", "wait", "dump"]
    h.run(mixer_ini(values), lines)
    rec = cm.evaluate(h.dumps[0])
    routes = {s: r for side in ("record_l", "record_r") for s, r in rec[side].items()}
    if set(routes) != {"mic"} or any(side is not None for _, side in routes["mic"]):
        fail(test, f"recorded during the test: {cm.describe(rec)}")
    elif cm.evaluate(h.dumps[-1]) != expected(clamp_values(values)):
        fail(test, "after the test:\n" + cm.describe(cm.evaluate(h.dumps[-1])))
    else:
        ok(test)

    # two holders: a test that ends while a DVR file plays does not end the player's hold
    test = f"{h.target} hold: two holders"
    values = direct_only(rec_mic=1, av=8, live_audio_volume=10)
    lines = codec_lines(base_codec(h.target)) + START + ["wait", "line_out 1", "wait", "mark", "hold_test 1", "hold 1",
                                                         "hold_test 0", "set mic 3", "apply"]
    lines += codec_lines(dict(cm.LIBMEDIA_RESET)) + [f"sleep {WATCH_MS * 12}", "mark", "hold 0", "mark", "dump"]
    batches, _, _, _, _ = h.run(mixer_ini(values), lines)
    during = batches[h.marks[0]:h.marks[1]]
    on_release = h.marks[2] - h.marks[1]
    if during != [HELD_BATCH] or on_release != 1:
        fail(test, f"{len(during)} batches while either held (want only the held batch), {on_release} on the last release (want 1)")
    elif cm.evaluate(h.dumps[-1]) != expected(clamp_values(dict(values, mic=3, phones_at_boot=1))):
        fail(test, "after the last release:\n" + cm.describe(cm.evaluate(h.dumps[-1])))
    else:
        ok(test)


def recorded_sources(state, dac):
    result = cm.evaluate(state, dac_active=dac)
    return set(result["record_l"]) | set(result["record_r"])


def test_transitions(h):
    for name, values, start, change, dac in TRANSITIONS:
        test = f"{h.target} transition '{name}'"
        # everything before the mark is applied (after a switch to an analog source); the change
        # must be one batch on its own
        lines = codec_lines(base_codec(h.target)) + START + ["wait", "line_out 1"] + start + ["wait", "dump", "mark"] + change + ["sleep 300", "dump"]
        batches, _, _, _, _ = h.run(mixer_ini(values), lines)
        before, after = h.dumps[0], h.dumps[-1]
        new = batches[h.marks[0]:]
        if len(new) != 1:
            fail(test, f"{len(new)} batches for the change")
            continue
        batch = new[0]
        state = dict(before)
        powered = [cm.line_out_powered(state, dac)]
        capture = [cm.capture_connected(state, dac)]
        recorded = [recorded_sources(state, dac)]
        for name_value in batch:
            state = cm.apply(state, [name_value])
            powered.append(cm.line_out_powered(state, dac))
            capture.append(cm.capture_connected(state, dac))
            recorded.append(recorded_sources(state, dac))
        if state != after:
            fail(test, "replaying the batch does not give the codec state")
            continue
        problems = []
        # a source that is recorded before and after the change is recorded all the way through
        kept = recorded[0] & recorded[-1]
        gap = next((i for i, r in enumerate(recorded) if not kept <= r), None)
        if gap is not None:
            problems.append(f"{sorted(kept - recorded[gap])} not recorded in between (after '{batch[gap - 1][0]}')")
        if powered[0] and powered[-1] and not all(powered):
            problems.append(f"line out unpowered in between (after '{batch[powered.index(False) - 1][0]}')")
        if capture[0] and capture[-1] and not all(capture):
            problems.append(f"recording without a source in between (after '{batch[capture.index(False) - 1][0]}')")
        if powered[0] and not powered[-1]:
            first_change = next((n for n, v in batch if before.get(n) != v), None)
            if first_change != "Lineout Switch":
                problems.append(f"line out goes off, but '{first_change}' changes before it")
        if problems:
            fail(test, "; ".join(problems))
        else:
            ok(test)


def test_worker(h):
    test = f"{h.target} worker"
    values = direct_only(rec_av=1, av=8, live_audio_volume=10)
    ini = mixer_ini(values)
    lines = codec_lines(base_codec(h.target)) + START + ["wait", "line_out 1", "wait", "mark", "amixer_delay 150",
                                                         "time_apply 20", "unsaved", "wait", "dump"]
    batches, queries, codec, _, after = h.run(ini, lines)
    slowest = int(query(queries, "APPLY_US ")[0].split()[1])
    burst = len(batches) - h.marks[0]  # the 20 steps and the final wait
    problems = []
    if slowest > 30000:
        problems.append(f"an apply call took {slowest / 1000:.0f} ms (amixer takes 150 ms)")
    if not 1 <= burst <= 4:
        problems.append(f"20 dial steps gave {burst} batches")
    if codec.get("lineout volume") != live_line_out(9):
        problems.append(f"the codec ended at line out {codec.get('lineout volume')}, not the last step ({live_line_out(9)})")
    if after != ini:
        problems.append("setting.ini written before audio_mixer_save()")
    if query(queries, "UNSAVED ") != ["UNSAVED 1"]:
        problems.append("changes not reported as unsaved")
    _, queries, _, _, after = h.run(ini, START + ["set live_audio_volume 7", "set av 5", "save", "unsaved"])
    if "live_audio_volume=7" not in after or "av=5" not in after or query(queries, "UNSAVED ") != ["UNSAVED 0"]:
        problems.append("audio_mixer_save() did not write the changes")
    # set() clamps like loading does (the page shows and saves what get() returns)
    _, queries, _, _, _ = h.run(ini, START + ["set mic 99", "set live_audio_volume 200", "set linein_gain -4", "set rec_av 5",
                                              "set audio_source 9", "get mic", "get live_audio_volume", "get linein_gain",
                                              "get rec_av", "get audio_source"])
    got = [q.split()[2] for q in query(queries, "GET ")]
    if got != ["8", "10", "0", "1", "2"]:
        problems.append(f"set() does not clamp: mic, live, linein_gain, rec_av, audio_source read back as {got}")
    if problems:
        fail(test, "; ".join(problems))
    else:
        ok(test)

    # hold: ripples' playback / a test drives the codec; nothing written meanwhile (also not by the
    # watcher, whatever ripples' code sets), the state applied on release, then checked once more
    test = f"{h.target} hold"
    lines = codec_lines(base_codec(h.target)) + START + ["wait", "line_out 1", "wait", "dump", "mark", "hold 1",
                                                         "set mic 5", "apply", "wait", "line_out 0", "line_out 1"]
    lines += codec_lines(dict(cm.LIBMEDIA_RESET, **{"Left Output Mixer LINEINL Switch": 0, "Right Output Mixer LINEINR Switch": 0}))
    lines += [f"sleep {WATCH_MS * 12}", "mark", "hold 0", "mark", "dump",
              "codec lineout volume=27", f"sleep {WATCH_MS * 3}", "mark"]
    batches, queries, codec, _, _ = h.run(mixer_ini(direct_only(rec_av=1, av=8, live_audio_volume=10)), lines)
    during = batches[h.marks[0]:h.marks[1]]
    on_release = h.marks[2] - h.marks[1]
    watched = h.marks[3] - h.marks[2]
    want = expected(clamp_values(direct_only(rec_av=1, av=8, mic=5, live_audio_volume=10)))
    if during != [HELD_BATCH] or on_release != 1 or watched != 1 or query(queries, "WAIT ")[-1] != "WAIT ok":
        fail(test, f"{len(during)} batches while held (want only the held batch), {on_release} on release, "
                   f"{watched} for a reset afterwards (want 1, 1); {query(queries, 'WAIT ')}")
    elif cm.evaluate(h.dumps[-1]) != want:
        fail(test, "after release:\n" + cm.describe(cm.evaluate(h.dumps[-1])) + "\n want:\n" + cm.describe(want))
    else:
        ok(test)

    # hold() taken while the worker has taken a state but not yet written it (playback or a test starting
    # just then): that batch is not written while held, and the state is applied on release
    test = f"{h.target} hold, taken right before a batch"
    lines = codec_lines(base_codec(h.target)) + START + ["wait", "line_out 1", "wait", "mark", "hold_before_batch",
                                                         "set mic 5", "apply", f"sleep {WATCH_MS * 4}", "mark", "hold 0",
                                                         "mark", "dump"]
    batches, queries, codec, _, _ = h.run(mixer_ini(direct_only(rec_av=1, av=8, live_audio_volume=10)), lines)
    during = batches[h.marks[0]:h.marks[1]]
    on_release = h.marks[2] - h.marks[1]
    if during != [HELD_BATCH] or on_release != 1:
        fail(test, f"{len(during)} batches while held (want only the held batch), {on_release} on release (want 1)")
    elif cm.evaluate(h.dumps[-1]) != want:
        fail(test, "after release:\n" + cm.describe(cm.evaluate(h.dumps[-1])) + "\n want:\n" + cm.describe(want))
    else:
        ok(test)


def test_watcher(h):
    # a record daemon starts (its pid file), then its AW_MPI_SYS_Init resets part of the mixer: undone.
    # Also the daemon that already runs when the app starts (boot): its window opens at the first look.
    values = direct_only(rec_line_in=1, av=8, live_audio_volume=10)
    for name, before_start, restart in (("record daemon start", [], ["recorder 2"]),
                                        ("record daemon running at boot", ["recorder 7"], [])):
        test = f"{h.target} watcher, {name}"
        lines = codec_lines(base_codec(h.target, libmedia=False)) + before_start + START + ["wait", f"sleep {WATCH_MS * 3}",
                                                                                              "dump", "mark"]
        lines += restart + codec_lines(dict(cm.LIBMEDIA_RESET)) + [f"sleep {WATCH_MS * 8}", "dump"]
        batches, _, _, log, _ = h.run(mixer_ini(values), lines)
        before, after = h.dumps[0], h.dumps[-1]
        reapplied = len(batches) - h.marks[0]
        if after != before or reapplied != 1 or "codec changed by someone else" not in log or "record daemon" not in log:
            fail(test, f"reset not undone: {reapplied} batches, "
                       f"differences {sorted(k for k in after if after[k] != before.get(k))}")
        else:
            ok(test)

    # the clicks of juricabi.2 / .3: nothing may be read while nothing can have changed the codec (after
    # the apply's one pass over every control of the batch, and after the boot daemon's window)
    for name, before_start, settle in (("after an apply", [], WATCH_MS * 3),
                                       ("after the boot daemon's window", ["recorder 7"], RECHECK_WINDOW + WATCH_MS * 3)):
        test = f"{h.target} watcher, idle {name}: the codec is not read"
        lines = codec_lines(base_codec(h.target, libmedia=False)) + before_start + START + ["wait", f"sleep {settle}", "reads",
                                                                                              f"sleep {WATCH_MS * 32}", "reads"]
        batches, queries, _, _, _ = h.run(mixer_ini(values), lines)
        (r0, a0), (r1, a1) = [tuple(int(x) for x in q.split()[1:]) for q in query(queries, "READS ")]
        if r1 != r0 or a1 != a0 or len(batches) != 1:
            fail(test, f"{r1 - r0} controls read and {a1 - a0} amixer runs in {WATCH_MS * 32} ms with nothing changed")
        elif not before_start and r0 != len(batches[0]):
            fail(test, f"{r0} controls read after the apply, want one pass over the batch's {len(batches[0])}")
        else:
            ok(test)

    test = f"{h.target} watcher, control the codec does not keep"
    lines = codec_lines(base_codec(h.target, libmedia=False)) + ["stuck lineout volume=27"] + START + ["wait",
                                                                   f"sleep {WATCH_MS * 40}", "mark", "set av 4", "apply", "wait"]
    batches, _, _, log, _ = h.run(mixer_ini(values), lines)
    # 1 batch, 3 retries at once, then again with a backoff (8, 16, 32 ... periods): in 40 periods
    # 2 or 3 more, never a loop, never given up; a new state is applied at once (1 more)
    before = h.marks[0]
    if not 5 <= before <= 7 or len(batches) != before + 1 or log.count("does not keep the routing") != 1:
        fail(test, f"{before} batches in {WATCH_MS * 40} ms before the change, {len(batches)} in all, "
                   f"'does not keep' logged {log.count('does not keep the routing')} times")
    else:
        ok(test)

    # a control that no libmedia_mpp reset writes, changed: every pass reads the whole batch, the one
    # after an apply, the one at once after audio_mixer_recheck() and the one after a daemon start
    for name, idle, trigger, within in (("after an apply", [], [], WATCH_MS * 2),
                                        ("recheck", [f"sleep {WATCH_MS * 3}"], ["recheck"], WATCH_MS),
                                        ("record daemon start", [f"sleep {WATCH_MS * 3}"], ["recorder 5"], WATCH_MS * 2 + 30)):
        test = f"{h.target} watcher, other control changed, {name}"
        lines = codec_lines(base_codec(h.target, libmedia=False)) + START + ["wait"] + idle + [
            "mark", "codec RADC input Mixer MIC2 boost Switch=0"]
        lines += trigger + [f"sleep {within}", "mark", "dump"]
        batches, _, codec, _, _ = h.run(mixer_ini(values), lines)
        if h.marks[1] - h.marks[0] != 1 or codec.get("RADC input Mixer MIC2 boost Switch") != 1:
            fail(test, f"{h.marks[1] - h.marks[0]} batches within {within} ms")
        else:
            ok(test)
    test = f"{h.target} watcher, recheck during a backoff"
    lines = codec_lines(base_codec(h.target, libmedia=False)) + START + ["wait", "dump", "codec_fail 1",
                                                                         f"sleep {WATCH_MS * 3}", "codec_fail 0"]
    lines += codec_lines(dict(cm.LIBMEDIA_RESET)) + ["mark", "recheck", f"sleep {WATCH_MS}", "mark", "dump"]
    h.run(mixer_ini(values), lines)
    before, after = h.dumps[0], h.dumps[-1]
    if h.marks[1] - h.marks[0] != 1 or after != before:
        fail(test, f"{h.marks[1] - h.marks[0]} batches within {WATCH_MS} ms of the recheck, "
                   f"differences {sorted(k for k in after if after[k] != before.get(k))}")
    else:
        ok(test)

    # a codec that cannot be read: logged once, looked at again with a backoff, watched again
    test = f"{h.target} watcher, codec not readable"
    lines = codec_lines(base_codec(h.target, libmedia=False)) + START + ["wait", "codec_fail 1", "sleep 20", "reads",
                                                                         f"sleep {WATCH_MS * 40}", "reads", "codec_fail 0",
                                                                         f"sleep {WATCH_MS * 40}", "mark"]
    lines += ["recorder 3"] + codec_lines(dict(cm.LIBMEDIA_RESET)) + [f"sleep {WATCH_MS * 4}", "mark"]
    batches, queries, _, log, _ = h.run(mixer_ini(values), lines)
    (r0, _), (r1, _) = [tuple(int(x) for x in q.split()[1:]) for q in query(queries, "READS ")]
    if log.count("cannot read the codec") != 1 or "has the routing" not in log:
        fail(test, "not logged once, or not watched again")
    elif r1 - r0 > 6:
        fail(test, f"{r1 - r0} reads in {WATCH_MS * 40} ms while the codec cannot be read (no backoff)")
    elif h.marks[1] - h.marks[0] != 1:
        fail(test, "a reset after the codec became readable again was not undone")
    else:
        ok(test)

    # amixer failing: logged, the routing applied again with a backoff until it is in the codec
    test = f"{h.target} watcher, amixer fails"
    lines = codec_lines(base_codec(h.target, libmedia=False)) + ["amixer_fail 5"] + START + ["wait",
                                                                   f"sleep {WATCH_MS * 60}", "reads", "dump"]
    batches, queries, codec, log, _ = h.run(mixer_ini(values), lines)
    runs = int(query(queries, "READS ")[0].split()[2])
    want = cm.evaluate(dict(base_codec(h.target, libmedia=False), **dict(batches[-1]))) if batches else None
    if "amixer failed" not in log or len(batches) != 1 or runs != 6 or cm.evaluate(codec) != want:
        fail(test, f"{runs} amixer runs, {len(batches)} succeeded; logged: {'amixer failed' in log}")
    else:
        ok(test)
    test = f"{h.target} watcher, amixer fails, codec not readable"
    lines = codec_lines(base_codec(h.target, libmedia=False)) + ["codec_fail 1", "amixer_fail 3"] + START + ["wait",
                                                                   f"sleep {WATCH_MS * 12}", "reads"]
    batches, queries, _, _, _ = h.run(mixer_ini(values), lines)
    runs = int(query(queries, "READS ")[0].split()[2])
    if runs != 4 or len(batches) != 1:
        fail(test, f"{runs} amixer runs, {len(batches)} succeeded (want 4, the last one)")
    else:
        ok(test)


def test_daemon_restart(h):
    """The record daemon's AW_MPI_SYS_Init switches the line out on at 27 (SD check at boot, card
    insertion, SD repair / format). While the mixer has the line out off, nothing may then be heard
    that the recording of the mix does not need; the watcher, which sees the daemon's new pid file,
    puts the routing back within two periods."""
    for audio_source in (0, 1, 2):
        old = migration_spec(audio_source)
        for name, values, start in (
                ("boot into HDZero (stock: silent)", old, []),
                ("HDMI in", old, ["line_out 0"]),
                ("Headphone Volume 0 on an analog source", dict(old, live_audio_volume=0, phones_at_boot=1), ["line_out 1"]),
                ("HDMI in, mic heard and recorded", dict(old, rec_mic=1, mic=4), ["line_out 0"]),
                ("HDMI in audio, Headphone Volume 0", dict(old, hdmi=4, live_audio_volume=0), ["line_out 0", "live_dac 1"]),
                ("HDMI in audio, Headphone Volume 0, nothing recorded", dict(old, rec_mic=0, rec_line_in=0, rec_av=0, hdmi=4, live_audio_volume=0),
                 ["line_out 0", "live_dac 1"])):
            test = f"{h.target} daemon restart, Audio Source {audio_source}, {name}"
            lines = codec_lines(base_codec(h.target)) + START + ["apply", "wait"] + start + ["wait", f"sleep {WATCH_MS * 3}",
                                                                                              "dump", "mark"]
            lines += ["recorder 9"] + codec_lines(dict(cm.LIBMEDIA_RESET)) + [f"sleep {WATCH_MS * 2 + 30}", "mark", "dump"]
            batches, _, _, _, _ = h.run(mixer_ini(values), lines)
            before, after = h.dumps[0], h.dumps[-1]
            dac = "live_dac 1" in start
            reset = cm.daemon_restart(before)
            allowed = cm.recorded_via_mix(before, dac_active=dac)
            if dac:
                # The reset itself switches the left DAC into the output mixer (LIBMEDIA_RESET), which no
                # mixer setting can prevent: here only the state before it shows the rule.
                allowed |= {"dac"}
                on = sorted(k for k, v in before.items() if "Output Mixer" in k and k.endswith("Switch") and v)
                if on:
                    fail(test, f"line out off and no mix, yet in the output mixers: {on}")
                    continue
            noise = cm.heard(reset, dac_active=dac) - allowed
            if cm.heard(before, dac_active=dac):
                fail(test, f"line out not off before the reset: {cm.heard(before, dac_active=dac)}")
            elif noise:
                fail(test, f"after the reset the headphones get {sorted(noise)}")
            elif after != before or h.marks[1] - h.marks[0] != 1:
                fail(test, f"the reset not undone within {WATCH_MS * 2} ms ({h.marks[1] - h.marks[0]} batches)")
            else:
                ok(test)


def test_driver_routes(path):
    src = open(path, encoding="utf-8", errors="replace").read()
    table = src[src.index("ac_dapm_routes[]"):]
    table = table[:table.index("};")]
    driver = set()
    for m in re.finditer(r'\{\s*"([^"]+)",\s*(NULL|"[^"]*"),\s*"([^"]+)"\s*\}', table):
        driver.add((m.group(1), None if m.group(2) == "NULL" else m.group(2).strip('"'), m.group(3)))
    missing = [r for r in cm.ROUTES if r not in driver]
    if missing:
        fail("model vs driver routes", f"not in the driver: {missing}")
    else:
        ok(f"model: all {len(cm.ROUTES)} routes are the driver's")


def test_model_facts():
    """What the driver's routes allow for per-side record levels (docs/audio.md)."""
    state = {"AIF1 AD0L Mixer ADCL Switch": 1, "AIF1 AD0R Mixer ADCR Switch": 1,
             "LADC input Mixer MIC1 boost Switch": 1, "RADC input Mixer LINEINR Switch": 1}
    split = cm.evaluate(state)
    if set(split["record_l"]) != {"mic"} or set(split["record_r"]) != {"av"}:
        fail("model: split stereo", cm.describe(split))
    for item in (2, 3):  # SUM / AVE: no route, the mux (and the slot channel) is disconnected
        result = cm.evaluate(dict(state, **{"AIF1OUT0L Mux": item, "AIF1OUT0R Mux": item}))
        if result["record_l"] or result["record_r"]:
            fail("model: SUM/AVE mux", f"item {item} still connects: {cm.describe(result)}")
    if any(sink == "AIF1 AD0L Mixer" and source == "RADC input Mixer" for sink, _, source in cm.ROUTES):
        fail("model: cross-mix", "left AIF1 slot can take the right ADC")
    ok("model: split stereo possible, codec-side SUM/AVE and cross-mix not")


def main():
    global verbose
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--src", default=os.path.join(ROOT, "src"), help="directory with core/audio_mixer*.c")
    parser.add_argument("--driver", help="sun8iw16-codec.c of the BSP: check the modelled routes")
    parser.add_argument("--targets", default=",".join(TARGETS))
    parser.add_argument("--only", help="run only the tests whose function name contains this")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    verbose = args.verbose

    if args.driver:
        test_driver_routes(args.driver)
    test_model_facts()

    work = tempfile.mkdtemp(prefix="audio_mixer_test_")
    try:
        for target in args.targets.split(","):
            try:
                h = Harness(target, os.path.abspath(args.src), work)
            except subprocess.CalledProcessError as e:
                fail(target, f"build failed: {e}")
                continue
            for test in (test_routing, test_loading, test_settings, test_migration, test_phones_at_boot,
                         test_transitions, test_worker, test_hold, test_watcher, test_daemon_restart):
                if args.only and args.only not in test.__name__:
                    continue
                try:
                    test(h)
                except Exception as e:  # a crashing mutant is a failure too
                    fail(f"{target} {test.__name__}", repr(e))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if failures:
        print(f"{len(failures)} FAILED")
        return 1
    print("audio mixer test: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
