#!/usr/bin/env python3
"""Checks every string of the Audio page (ripples' rows and the mixer's extras), the Module Bay page
and the new WiFi rows in every language with the firmware's own fonts:

  - translated in every mkapp/app/language/*.ini (ripples' own strings that ripples 1.2.2 has not
    translated, RIPPLES_UNTRANSLATED, are shown in English as on ripples: measured in English),
  - every character present in the font (the CJK glyphs of the fonts are only those of the
    translations the fonts were generated from, utilities/font/generate_font_lib.py),
  - as wide as its column at most (conf/ui.h UI_AUDIO_*, page_modulebay.c), on the goggles and
    (Audio and WiFi pages) on the BoxPro; texts the firmware puts together from several keys
    are measured put together, in their widest form.

    utilities/audio_mixer/check_strings.py [-v]

Needs a host C compiler; builds LVGL for the host once (about a minute) in a temporary directory.
Exit status 1 on any problem, also when the measuring program fails or answers too few lines.
"""

import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
PAGE = os.path.join(ROOT, "src/ui/page_audio.c")
LANG_DIR = os.path.join(ROOT, "mkapp/app/language")


def page_strings():
    """(kind, English text) of every string the page shows, from page_audio.c."""
    src = open(PAGE, encoding="utf-8").read()
    out = []
    for m in re.finditer(r'^\s*HEADING\("([^"]+)"\)', src, re.M):
        out.append(("heading", m.group(1)))
    for m in re.finditer(r'^\s*(?:SLIDER|YES_NO)\([^,"]+,\s*"([^"]+)",\s*"([^"]+)"\)', src, re.M):
        out += [("label", m.group(1)), ("hint", m.group(2))]
    for m in re.finditer(r'^\s*ON_OFF\([^,"]+,\s*[^,"]+,\s*"([^"]+)",\s*"([^"]+)"\)', src, re.M):
        out += [("label", m.group(1)), ("hint", m.group(2))]
    for m in re.finditer(r'^\s*RECORD_SOURCE\([^,"]+,\s*"([^"]+)"\)', src, re.M):
        out.append(("label", m.group(1)))
    # ripples' rows (explicit initializers): their names and hints
    for m in re.finditer(r'^\s*\{\.type = ROW_\w+, \.field = \w+, \.name = "([^"]+)"(?:, \.hint = "([^"]+)")?', src, re.M):
        if m.group(1) != "Back":
            out.append(("label", m.group(1)))
        if m.group(2):
            out.append(("hint", m.group(2)))
    for m in re.finditer(r'^#define HINT_\w+\s+"([^"]+)"', src, re.M):
        out.append(("hint", m.group(1)))
    # ripples' note of the test buttons: a key per line, each shown as a line of its own in ripples' font;
    # the first array for Goggle 2 / BoxPro, the second (#else) for the Goggle 1 only
    arrays = re.findall(r'^static const char \*const HINT_TEST\[\] = \{(.*?)\};', src, re.M | re.S)
    if len(arrays) != 2:
        raise SystemExit(f"page_audio.c: {len(arrays)} HINT_TEST arrays, want 2 (Goggle 2 / BoxPro, Goggle 1)")
    first = re.findall(r'"([^"]+)"', arrays[0])
    out += [("note", line) for line in first]
    out += [("note_goggle1", line) for line in re.findall(r'"([^"]+)"', arrays[1]) if line not in first]
    # a test that recorded nothing: its message and the two rows that hold the input
    busy = re.search(r'^#define HINT_TEST_BUSY\s+"([^"]+)"', src, re.M).group(1)
    out.append(("hint", ("{} ({}, {})", (busy, "Stream Audio", "HDMI Out Audio"))))
    # ripples' Audio Source (its label and three buttons as one row) and test buttons
    out.append(("source_row", ("{}|{}|{}|{}", ("Audio Source", "Mic", "Line In", "A/V In"))))
    out += [("test_button", b) for b in ("DVR", "Live", "Mic", "Line/AV")]
    if '.name = "Back"' in src:
        out.append(("label", "< Back"))
    for m in re.finditer(r'^#define MARK_\w+\s+"([^"]+)"', src, re.M):
        out.append(("value", m.group(1)))
    out += [("button", b) for b in ("Yes", "No", "On", "Off")]
    out += [("value", "Off"), ("value", "Mix")] + [("value", v) for v in NUMBERS]
    # "Off at this start: <reason>" (row_hint(), core/hdmi_audio.c's reasons)
    fault = re.search(r'^#define HINT_HDMI_FAULT\s+"([^"]+)"', src, re.M).group(1)
    out += [("hint", ("{}: {}", (fault, reason))) for reason in HDMI_FAULTS]
    return sorted(set(out), key=str)


NUMBERS = ["+6.0 dB", "-4.5 dB", "10"]  # widest values, shown as they are in every language
# ripples' own strings that ripples 1.2.2's language files do not translate are translated here since
# phase 4 (its nine labels and its note): none is left in English
RIPPLES_UNTRANSLATED = set()
HDMI_FAULTS = ["pin check failed", "HDMI chip check failed", "audio path failed", "probe not run"]

# The Module Bay page (src/ui/page_modulebay.c, src/module/*.c): (kind, text) or (kind, (format,
# keys)) for a text put together from translated parts. Goggles only.
MODULE_BAY = [("mb_label", t) for t in ("Module", "Mode", "Check Pins")] + [("mb_label", "< Back")] + \
    [("mb_option", t) for t in ("None", "Switch (Fat Shark)", "rapidFIRE", "SteadyView", "SteadyView X",
                                "Wiring A (standard)", "Wiring B", "Wiring C", "Wiring D", "Wiring E", "Wiring F",
                                "Diversity", "Mix 1", "Mix 2", "Mix 3")] + \
    [("mb_text", t) for t in ("Click to read the 3 bay pins (read only)", "Reading...", "Mode needs the bay power switch",
                              "Not read: set Module to None first", "Not read: switch WiFi off first",
                              "Not read: pins in use by WiFi", "Not read: pin state unknown", "Not read: no GPIO access")] + \
    [("mb_text", ("{} {} {} {}, 1.8 V", ("Pins", "toggling", "toggling", "toggling"))),
     ("mb_text", ("{} 1 0 1, 3.3 V", ("Pins",)))] + \
    [("mb_status", ("{}: {}", ("Status", t))) for t in ("Off, bay pins untouched", "Paused while WiFi is on",
                                                        "Paused, pins in use by WiFi", "Paused, pin state unknown",
                                                        "Waiting for bay power",
                                                        "Waiting for the module to boot")] + \
    [("mb_status", ("{}: {} SteadyView X L8 {}", ("Status", "Sending", "(not video source)")))] + \
    [("mb_note", t) for t in ("Module control needs the 3-wire bay mod (resistors or level shifter).",
                              "It pauses while WiFi is on: WiFi uses the same pins.",
                              "Furious True-D with MOD 1.17: choose rapidFIRE.")]

# The WiFi page's System rows of this build (src/ui/page_wifi.c).
WIFI = [("wifi_label", "Stream Audio"), ("wifi_note", ("{}.", ("Stream Audio: sound set on the Audio page",)))]


def is_ripples_untranslated(text):
    return isinstance(text, str) and text in RIPPLES_UNTRANSLATED


def translations(path):
    table = {}
    for line in open(path, encoding="utf-8"):
        m = re.match(r'^([^;#\[][^=]*?)\s*=\s*"(.*)"\s*$', line)
        if m:
            table[m.group(1)] = m.group(2)
    return table


def translate(text, table):
    if isinstance(text, tuple):  # (format, keys): put together from translated parts
        fmt, keys = text
        parts = [table.get(k) for k in keys]
        return None if None in parts else fmt.format(*parts)
    if text.startswith("< "):  # "< %s" of the Back row
        inner = table.get(text[2:])
        return None if inner is None else "< " + inner
    return table.get(text)


def english(text):
    if isinstance(text, tuple):
        return text[0].format(*text[1])
    return text


def keys_of(text):
    if isinstance(text, tuple):
        return list(text[1])
    return [text[2:] if text.startswith("< ") else text]


def build(work):
    objs = os.path.join(work, "lvgl")
    os.makedirs(objs)
    sources = glob.glob(os.path.join(ROOT, "lib/lvgl/lvgl/src/**/*.c"), recursive=True)
    cflags = ["-std=gnu99", "-w", "-O1", "-DHDZGOGGLE2=1", "-I", os.path.join(ROOT, "lib/lvgl"),
              "-I", os.path.join(ROOT, "lib/lvgl/lvgl")]
    procs, outs = [], []
    for i, s in enumerate(sources):
        o = os.path.join(objs, f"{i}.o")
        outs.append(o)
        procs.append(subprocess.Popen(["cc", *cflags, "-c", s, "-o", o]))
        if len(procs) >= os.cpu_count() * 2:
            if procs.pop(0).wait():
                raise RuntimeError("LVGL build failed")
    for p in procs:
        if p.wait():
            raise RuntimeError("LVGL build failed")
    lib = os.path.join(work, "liblvgl.a")
    subprocess.run(["ar", "rcs", lib, *outs], check=True)
    tools = {}
    for target in ("HDZGOGGLE2", "HDZBOXPRO"):
        exe = os.path.join(work, f"check_{target}")
        p = subprocess.run(["cc", "-std=gnu99", "-Wall", "-Werror", f"-D{target}=1", "-I", os.path.join(ROOT, "lib/lvgl"),
                            "-I", os.path.join(ROOT, "src"), os.path.join(HERE, "check_strings.c"), lib,
                            "-o", exe, "-lm"], capture_output=True, text=True)
        if p.returncode:
            raise RuntimeError(f"check_strings.c ({target}) does not build:\n{p.stderr[-3000:]}")
        tools[target] = exe
    return tools


def main():
    verbose = "-v" in sys.argv
    strings = page_strings()
    langs = {"en": {}}
    for path in sorted(glob.glob(os.path.join(LANG_DIR, "*.ini"))):
        langs[os.path.basename(path)[:-4]] = translations(path)

    problems = []
    # every entry of a language file must fit the firmware's table (src/lang/language.c drops the rest)
    limit = int(re.search(r"#define TRANSLATE_STRING_NUM\s+(\d+)",
                          open(os.path.join(ROOT, "src/lang/language.h")).read()).group(1))
    for path in sorted(glob.glob(os.path.join(LANG_DIR, "*.ini"))):
        n = sum(1 for line in open(path, encoding="utf-8") if re.match(r'^[^;#\[\s][^=]*=', line))
        print(f"{os.path.basename(path)}: {n} of {limit} translation entries")
        if n > limit:
            problems.append(f"{os.path.basename(path)} has {n} entries, the table {limit}")
    def lines_of(strings):
        out = []
        for lang, table in langs.items():
            for kind, text in strings:
                # a line of ripples' note is keyed with '-' for '=' (page_audio.c note_line())
                key = text.replace("=", "-") if kind.startswith("note") else text
                shown = english(text) if lang == "en" or text in NUMBERS else translate(key, table)
                if shown is None and is_ripples_untranslated(text):
                    shown = english(text)  # as on ripples
                if shown is None:
                    missing = [k for k in keys_of(key) if k not in table]
                    problems.append(f"not translated in {lang}: {missing}")
                    shown = english(text)  # the firmware shows the English text
                if kind.startswith("note"):
                    out.append(f"note\t{lang}\t{shown}")
                else:
                    out.append(f"{kind}\t{lang}\t{shown}")
        return out

    # the Module Bay page exists on the goggles only
    per_target = {"HDZGOGGLE2": strings + MODULE_BAY + WIFI,
                  "HDZBOXPRO": [s for s in strings if s[0] != "note_goggle1"] + WIFI}
    lines = {target: lines_of(s) for target, s in per_target.items()}
    problems = sorted(set(problems), key=problems.index)

    work = tempfile.mkdtemp(prefix="audio_strings_")
    try:
        tools = build(work)
        widest = {}
        for target, exe in tools.items():
            p = subprocess.run([exe], input="\n".join(lines[target]) + "\n", capture_output=True, text=True)
            answers = p.stdout.splitlines()
            bad = 0
            for out in answers:
                verdict, kind, lang, size = out.split(" ")[:4]
                if verdict == "BAD-KIND":
                    problems.append(f"{target}: {out}")
                    bad += 1
                    continue
                used, room = (int(x) for x in size.split("/"))
                key = (target, kind)
                if used * 100 // room > widest.get(key, (0, ""))[0]:
                    widest[key] = (used * 100 // room, out)
                if verdict != "OK":
                    problems.append(f"{target}: {out}")
                    bad += 1
                elif verbose:
                    print(f"{target}: {out}")
            # the measuring program's own verdict must agree with its lines: a crash or a line it
            # did not answer is a failure, not a pass
            if len(answers) != len(lines[target]) or p.returncode != (1 if bad else 0):
                problems.append(f"{target}: the measuring program exited with {p.returncode} after "
                                f"{len(answers)} of {len(lines[target])} lines: {p.stderr[-300:]}")
        for (target, kind), (pct, out) in sorted(widest.items()):
            print(f"widest {kind:10s} {target:10s} {pct:3d} %: {out}")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    for p in problems:
        print("FAIL " + p)
    count = sum(len(s) for s in per_target.values())
    print(f"{count} strings (both layouts) x {len(langs)} languages: "
          + ("all fit" if not problems else f"{len(problems)} problems"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
