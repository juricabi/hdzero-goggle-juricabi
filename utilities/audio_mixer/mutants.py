#!/usr/bin/env python3
"""Mutation check of test_mixer.py on ripples-all: every mutant of the mixer sources must make it fail.

    utilities/audio_mixer/mutants.py [--targets HDZGOGGLE2,...] [name filter]

Each mutant is one small change to a copy of src/core/audio_mixer*.c; test_mixer.py runs against
the copy (--src). all's mutants (082b2f5) that still apply, the ones of all's reviews among them,
and the ones of ripples-all's own rules (ripples' controls, the presets, the settings both ways,
hold), and the fixes of the review of phase 3 (the held batch, holders, the timeslot volumes, the round
trip through stock ripples). Exit status 1 if a mutant survives (or the unmodified copy fails).
"""

import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
FILES = ["audio_mixer.c", "audio_mixer_routing.c", "audio_mixer.h"]
R = "audio_mixer_routing.c"
M = "audio_mixer.c"

MUTANTS = [
    # ---- all's (082b2f5), on this code
    ("review: ADC MIC1 and LINEIN switches swapped", R, [
        ('{"LADC input Mixer MIC1 boost Switch", adc_mic}', '{"LADC input Mixer MIC1 boost Switch", adc_av}'),
        ('{"LADC input Mixer LINEINL Switch", adc_av}', '{"LADC input Mixer LINEINL Switch", adc_mic}')]),
    ("review: AIF1 AD0L/R ADC switches off", R, [
        ('{"AIF1 AD0L Mixer ADCL Switch", 1}', '{"AIF1 AD0L Mixer ADCL Switch", 0}'),
        ('{"AIF1 AD0R Mixer ADCR Switch", 1}', '{"AIF1 AD0R Mixer ADCR Switch", 0}')]),
    ("review: Record Headphone Mix inverted", R, [
        ("const bool rec_mix = v[AUDIO_MIXER_REC_MIX] != 0;", "const bool rec_mix = v[AUDIO_MIXER_REC_MIX] == 0;")]),
    ("mix + direct: a heard mic recorded twice", R, [
        ("const int adc_mic = v[AUDIO_MIXER_REC_MIC] && !(rec_mix && mon_mic > 0);",
         "const int adc_mic = v[AUDIO_MIXER_REC_MIC];")]),
    ("mix + direct: direct a/v only without the mix (old Custom)", R, [
        ("const int adc_av = v[AUDIO_MIXER_REC_AV] && !(rec_mix && mon_av > 0);",
         "const int adc_av = !rec_mix && v[AUDIO_MIXER_REC_AV];")]),
    ("mix + direct: line in left out whenever the mix is on", R, [
        ("const int adc_line = v[AUDIO_MIXER_REC_LINE_IN] && !(rec_mix && mon_line > 0);",
         "const int adc_line = v[AUDIO_MIXER_REC_LINE_IN] && !rec_mix;")]),
    ("page marker: level 0 counted as in the mix", R, [
        ("settings->value[AUDIO_MIXER_REC_MIX] && settings->value[level] > 0;",
         "settings->value[AUDIO_MIXER_REC_MIX] && settings->value[level] + 1 > 0;")]),
    ("migration: Record Headphone Mix on", R, [("v[AUDIO_MIXER_REC_MIX] = 0;", "v[AUDIO_MIXER_REC_MIX] = 1;")]),
    ("load: a version 1 [mixer] read as it is", M, [
        ("if (version < AUDIO_MIXER_VERSION) {", "if (version < 1) {")]),
    ("boot: headphones live from boot when untouched", M, [
        ("context.line_out = mixer.value[AUDIO_MIXER_PHONES_AT_BOOT] != 0;", "context.line_out = true;")]),
    ("boot: a headphone level change does not make them live", M, [
        ("        mixer.value[AUDIO_MIXER_PHONES_AT_BOOT] = 1;\n        if (!line_out_by_source)\n            context.line_out = true;\n",
         "        mixer.value[AUDIO_MIXER_PHONES_AT_BOOT] = 1;\n")]),
    ("boot: any change makes the headphones live", M, [
        ("    return field == AUDIO_MIXER_MIC || field == AUDIO_MIXER_LINE_IN",
         "    return field != AUDIO_MIXER_PHONES_AT_BOOT || field == AUDIO_MIXER_LINE_IN")]),
    ("load: a version 1 or cut-short section left behind", M, [
        ("            ini_puts(AUDIO_MIXER_SECTION, NULL, NULL, SETTING_INI);", "            (void)key;")]),
    ("load: [mixer] written at start-up (stock setting.ini)", M, [
        ("        char key[32];\n", "        ini_putl(AUDIO_MIXER_SECTION, \"version\", AUDIO_MIXER_VERSION, SETTING_INI);\n        char key[32];\n")]),
    ("save: the version never written", M, [("    if (wrote_mixer && !section_written) {", "    if (wrote_mixer && !section_written && 0) {")]),
    ("save: the version written first", M, [
        ("    bool wrote_mixer = false;\n", "    bool wrote_mixer = false;\n    if (!section_written)\n        ini_putl(AUDIO_MIXER_SECTION, \"version\", AUDIO_MIXER_VERSION, SETTING_INI);\n")]),
    ("switches in table order (off before on)", R, [
        ("        if (sw[i].on)\n            cset(&cb, sw[i].name, 1);", "        cset(&cb, sw[i].name, sw[i].on);"),
        ("        if (!sw[i].on)\n            cset(&cb, sw[i].name, 0);", "        if (0)\n            cset(&cb, sw[i].name, 0);")]),
    ("line out switched off last", R, [
        ('    if (!line_out)\n        cset(&cb, "Lineout Switch", 0);\n', ""),
        ('    if (line_out)\n        cset(&cb, "Lineout Switch", 1);', '    cset(&cb, "Lineout Switch", line_out);')]),
    ("line out not mono (LOMIX)", R, [('cset(&cb, "LINEOUTL Mux", 1);', 'cset(&cb, "LINEOUTL Mux", 0);')]),
    ("live dac: right DAC not in the mix", R, [
        ('{"Right Output Mixer DACR Switch", out_dac}', '{"Right Output Mixer DACR Switch", 0}')]),
    ("live dac: level around 160, not playback's 152", R, [("#define DAC_PLAYBACK       152", "#define DAC_PLAYBACK       160")]),
    ("migration: receiver audio always on", R, [
        ("v[AUDIO_MIXER_RX_AUDIO] = rec->audio_source == 2;", "v[AUDIO_MIXER_RX_AUDIO] = rec->audio_source == 2 || v[AUDIO_MIXER_AV] > 0;")]),
    ("migration: negative old value read as Mic", R, [
        ("uint8_t dvr_source = (uint8_t)rec->audio_source;", "uint8_t dvr_source = rec->audio_source < 0 ? 0 : (uint8_t)rec->audio_source;")]),
    ("live dac mutes the other sources", R, [("const int mon_mic = v[AUDIO_MIXER_MIC];",
                                              "const int mon_mic = context->live_dac ? 0 : v[AUDIO_MIXER_MIC];")]),
    ("live dac: line out stays off with hdmi in", R, [
        ("const int line_out = (context->line_out || live_dac) && volume > 0;",
         "const int line_out = context->line_out && volume > 0;")]),
    ("live dac: digital loopback into the ADC", R, [
        ('{"Right Output Mixer DACL Switch", 0},', '{"Right Output Mixer DACL Switch", 0},\n        {"AIF1 AD0L Mixer AIF1 DA0L Switch", dac_on},')]),
    ("load: no clamping", M, [
        ("mixer.value[i] = clamp_field(i, ini_getl(AUDIO_MIXER_SECTION, field_ini[i].key, defaults.value[i], SETTING_INI));",
         "mixer.value[i] = (uint8_t)ini_getl(AUDIO_MIXER_SECTION, field_ini[i].key, defaults.value[i], SETTING_INI);")]),
    ("load: missing fields from Mic, not ripples' state", M, [
        ("    audio_mixer_defaults(&defaults, rec);\n",
         "    audio_mixer_defaults(&defaults, rec);\n    if (ini_getl(AUDIO_MIXER_SECTION, \"version\", 0, SETTING_INI)) {\n        audio_mixer_record_t mic = *rec;\n        mic.audio_source = 0;\n        mic.live_volume = 10;\n        audio_mixer_defaults(&defaults, &mic);\n    }\n")]),
    ("set writes setting.ini", M, [
        ("    pthread_mutex_lock(&mixer_mutex);\n    set_locked(field, value);",
         "    pthread_mutex_lock(&mixer_mutex);\n    set_locked(field, value);\n    ini_putl(field_ini[field].section, field_ini[field].key, mixer.value[field], SETTING_INI);")]),
    ("apply in the caller's thread", M, [("    if (worker_running)\n        pthread_cond_signal(&work_cond);",
                                          "    if (0)\n        pthread_cond_signal(&work_cond);")]),
    ("no watcher", M, [("const int has = codec_has_last_batch(full);", "const int has = codec_has_last_batch(full) >= 0;")]),
    ("watcher: no backoff after the retries", M, [("#define AUDIO_MIXER_WATCH_RETRIES 3", "#define AUDIO_MIXER_WATCH_RETRIES 1000")]),
    ("review x3: ADC volume never written", R, [('    cset(&cb, "ADC volume", DIGITAL_VOLUME_OLD);\n', "")]),
    ("review x4: HDMI level is not a headphone level", M, [
        ("field == AUDIO_MIXER_AV || field == AUDIO_MIXER_HDMI;", "field == AUDIO_MIXER_AV;")]),
    ("review x5: unused output-mixer gains not reset to 0 dB", R, [
        ('    if (!out_mic)\n        cset(&cb, "MIC1_G boost stage output mixer control", OUTPUT_GAIN_0DB);\n', "")]),
    ("review x7: set() does not clamp", M, [
        ("    mixer.value[field] = clamp_field(field, value);\n    if (is_headphone_level",
         "    mixer.value[field] = (uint8_t)value;\n    if (is_headphone_level")]),
    ("line out off: sources stay in the output mixers", R, [
        ("const bool connect = line_out || rec_mix;", "const bool connect = true;")]),
    ("line out off: the DAC stays in the output mixers", R, [
        ("const int out_dac = connect && dac_on;", "const int out_dac = dac_on;")]),
    ("line out off: the recorded mix loses its sources", R, [
        ("const bool connect = line_out || rec_mix;", "const bool connect = line_out;")]),
    ("HDMI in: a level change switches the line out on", M, [
        ("        if (!line_out_by_source)\n            context.line_out = true;", "        context.line_out = true;")]),
    ("watcher: quick pass reads every control", M, [
        ("        if (!full && !is_reset_control(name))\n            continue;\n",
         "        if (0 && !full && !is_reset_control(name))\n            continue;\n")]),
    ("watcher: no full pass", M, [
        ("const bool full = recheck_now || rechecking || backoff || ++pass >= AUDIO_MIXER_WATCH_FULL;",
         "const bool full = recheck_now || rechecking || backoff || (++pass >= AUDIO_MIXER_WATCH_FULL && 0);")]),
    ("watcher: quick pass misses the line out volume", M, [('"MIC1 boost AMP gain control", "ADC input gain control", "ADC volume",',
                                                            '"MIC1 boost AMP gain control", "ADC input gain control", "ADC volume", "x",'),
                                                           ('"LADC input Mixer MIC1 boost Switch", "lineout volume", "DAC volume",',
                                                            '"LADC input Mixer MIC1 boost Switch", "DAC volume",')]),
    ("watcher: recheck ignored", M, [("    recheck_now = true;\n", "")]),
    ("watcher: stops for good when the codec cannot be read", M, [
        ("                unreadable = true;\n                backoff = next_backoff(backoff);",
         "                unreadable = true;\n                backoff = 1000000;")]),
    ("watcher: no backoff when the codec cannot be read", M, [
        ("                unreadable = true;\n                backoff = next_backoff(backoff);",
         "                unreadable = true;")]),
    ("amixer: exit status ignored", M, [("            ok = status == 0;", "            ok = true;")]),

    # ---- ripples' controls
    ("Live Volume: line out without ripples' rounding", R, [("    return (v * 31 + 5) / 10;", "    return (v * 31) / 10;")]),
    ("Live Volume: A/V curve without its minimum", R, [("    if (v > 0 && gain == 0)\n        gain = 1;\n", "")]),
    ("Live Volume: A/V level one step off", R, [("    return gain + 1;\n}", "    return gain;\n}")]),
    ("Live Volume slider leaves the A/V In level", M, [
        ("    mixer.value[AUDIO_MIXER_AV] = audio_mixer_live_av_level(mixer.value[AUDIO_MIXER_LIVE_VOLUME]);\n", "")]),
    ("Live Volume slider makes the headphones live from boot", M, [
        ("    mixer.value[AUDIO_MIXER_AV] = audio_mixer_live_av_level(mixer.value[AUDIO_MIXER_LIVE_VOLUME]);\n",
         "    mixer.value[AUDIO_MIXER_AV] = audio_mixer_live_av_level(mixer.value[AUDIO_MIXER_LIVE_VOLUME]);\n    mixer.value[AUDIO_MIXER_PHONES_AT_BOOT] = 1;\n    context.line_out = true;\n")]),
    ("ADC gain: Line/AV Gain also with the mic alone", R, [
        ("const int adc_gain = (rec_mix || adc_line || adc_av) ? v[AUDIO_MIXER_LINEIN_GAIN] : OUTPUT_GAIN_0DB;",
         "const int adc_gain = v[AUDIO_MIXER_LINEIN_GAIN];")]),
    ("ADC gain: 0 dB whatever is recorded", R, [
        ("const int adc_gain = (rec_mix || adc_line || adc_av) ? v[AUDIO_MIXER_LINEIN_GAIN] : OUTPUT_GAIN_0DB;",
         "const int adc_gain = OUTPUT_GAIN_0DB;")]),
    ("ADC gain: the mix not counted", R, [
        ("const int adc_gain = (rec_mix || adc_line || adc_av) ? v[AUDIO_MIXER_LINEIN_GAIN] : OUTPUT_GAIN_0DB;",
         "const int adc_gain = (adc_line || adc_av) ? v[AUDIO_MIXER_LINEIN_GAIN] : OUTPUT_GAIN_0DB;")]),
    ("Mic Gain on the MIC2 preamp", R, [
        ('cset(&cb, "MIC1 boost AMP gain control", v[AUDIO_MIXER_MIC_GAIN]);', 'cset(&cb, "MIC1 boost AMP gain control", v[AUDIO_MIXER_LINEIN_GAIN]);')]),
    ("Line/AV Gain not on the MIC2 preamp", R, [
        ('cset(&cb, "MIC2 boost AMP gain control", v[AUDIO_MIXER_LINEIN_GAIN]);', 'cset(&cb, "MIC2 boost AMP gain control", 4);')]),
    ("defaults: Mic Gain not ripples'", R, [
        ("v[AUDIO_MIXER_MIC_GAIN] = clamp(rec->mic_gain, AUDIO_MIXER_GAIN_MAX);", "v[AUDIO_MIXER_MIC_GAIN] = 4;")]),

    # ---- the presets of ripples' rows
    ("Audio Source: the other sources stay recorded", M, [
        ("    mixer.value[AUDIO_MIXER_REC_MIC] = s == 0;\n    mixer.value[AUDIO_MIXER_REC_LINE_IN] = s == 1;\n    mixer.value[AUDIO_MIXER_REC_AV] = s == 2;\n",
         "    mixer.value[AUDIO_MIXER_REC_MIC] |= s == 0;\n    mixer.value[AUDIO_MIXER_REC_LINE_IN] |= s == 1;\n    mixer.value[AUDIO_MIXER_REC_AV] |= s == 2;\n")]),
    ("Audio Source: the receiver's audio not set", M, [("    mixer.value[AUDIO_MIXER_RX_AUDIO] = s == 2;\n", "")]),
    ("Audio Source: marker ignores the other sources", M, [
        ("    const bool preset = mixer.value[AUDIO_MIXER_REC_MIC] == (s == 0) && mixer.value[AUDIO_MIXER_REC_LINE_IN] == (s == 1) &&\n                        mixer.value[AUDIO_MIXER_REC_AV] == (s == 2);",
         "    const bool preset = (s == 0 && mixer.value[AUDIO_MIXER_REC_MIC]) || (s == 1 && mixer.value[AUDIO_MIXER_REC_LINE_IN]) ||\n                        (s == 2 && mixer.value[AUDIO_MIXER_REC_AV]);")]),

    # ---- settings both ways
    ("ripples' values not handed back to g_setting", M, [("    rec->live_volume = mixer.value[AUDIO_MIXER_LIVE_VOLUME];\n", "")]),
    ("all's firmware: Live Volume not from its headphones", M, [
        ("    if (!had_live && has_key(AUDIO_MIXER_SECTION, \"headphones\")) {", "    if (0 && !had_live) {")]),
    ("all's firmware: headphones mapped without rounding", M, [
        ("(headphones * 10 + 15) / 31", "(headphones * 10) / 31")]),
    ("all's firmware: its headphones over ripples' own Live Volume", M, [
        ("    const bool had_live = has_key(RECORD_SECTION, \"live_audio_volume\") || has_key(RECORD_SECTION, \"audio_volume\");",
         "    const bool had_live = false;")]),
    ("all's firmware: Mic Gain not from its mic boost", M, [
        ("    if (!has_key(RECORD_SECTION, \"mic_gain\") && has_key(AUDIO_MIXER_SECTION, \"mic_boost\"))", "    if (0)")]),
    ("all's firmware: Line/AV Gain from its Line In boost", M, [
        ("ini_getl(AUDIO_MIXER_SECTION, \"dvr_level\", 3, SETTING_INI)", "ini_getl(AUDIO_MIXER_SECTION, \"line_in_boost\", 3, SETTING_INI)")]),
    ("all's firmware: DVR Playback not from its playback volume", M, [
        ("            rec->dvr_volume = dvr_volume_of_playback(", "            (void)dvr_volume_of_playback(")]),
    ("all's firmware: playback mapped to the nearest level below", M, [
        ("        if (line_out[level] >= playback)\n            return level;", "        if (line_out[level] > playback)\n            return level - 1;")]),
    ("all's firmware: Audio Source not from its Record switches", M, [
        ("        if (direct == 1 && file_basis_source == NO_KEY)\n", "        if (direct == 99 && file_basis_source == NO_KEY)\n")]),
    ("save: ripples' keys written without a change", M, [
        ("        if (current.value[i] != saved.value[i] || (stale & FIELD_BIT(i))) {",
         "        if (current.value[i] != saved.value[i] || (stale & FIELD_BIT(i)) || strcmp(field_ini[i].section, RECORD_SECTION) == 0) {")]),
    # ---- the round trip ripples-all -> stock ripples -> ripples-all (review of phase 3, finding 2)
    ("round trip: record_source ignored", M, [
        ("    if (file_basis_source != NO_KEY && file_basis_source != rec->audio_source) {",
         "    if (file_basis_source != NO_KEY && file_basis_source != rec->audio_source && 0) {")]),
    ("round trip: record_live ignored", M, [
        ("    if (file_basis_live != NO_KEY && file_basis_live != rec->live_volume) {",
         "    if (file_basis_live != NO_KEY && file_basis_live != rec->live_volume && 0) {")]),
    ("round trip: record_source not written", M, [
        ("            ini_putl(AUDIO_MIXER_SECTION, \"record_source\", file_basis_source, SETTING_INI);\n", "")]),
    ("round trip: record_live not written", M, [
        ("            ini_putl(AUDIO_MIXER_SECTION, \"record_live\", file_basis_live, SETTING_INI);\n", "")]),
    ("round trip: values that follow ripples not written with the next change", M, [
        ("        if (current.value[i] != saved.value[i] || (stale & FIELD_BIT(i))) {", "        if (current.value[i] != saved.value[i]) {")]),
    ("this firmware's [mixer]: the Audio Source from the Record switches (all's rule)", M, [
        ("        if (direct == 1 && file_basis_source == NO_KEY)\n", "        if (direct == 1)\n")]),
    ("save: a missing field that [record] would change not written", M, [
        ("            from_record.value[i] != current.value[i]) {", "            from_record.value[i] != current.value[i] && 0) {")]),
    ("save: [record] not brought in line with all's values", M, [
        ("        if (file_source != current.value[AUDIO_MIXER_SOURCE]) {", "        if (file_source != current.value[AUDIO_MIXER_SOURCE] && 0) {")]),

    # ---- hold (ripples' playback and test buttons)
    ("hold: batches written while held", M, [
        ("    if (held) {\n        // ripples' own code drives the codec", "    if (0) {\n        // ripples' own code drives the codec")]),
    ("hold: nothing applied on release", M, [("    if (released) {", "    if (released && 0) {")]),
    # (review of phase 3, findings 1, 4, 5, 7)
    ("hold: no held batch (the page's headphone sources heard with a DVR file)", M, [("    if (taken) {\n        // the mixer's own live paths out", "    if (taken && 0) {\n        // the mixer's own live paths out")]),
    ("held batch: the mic stays in the output mixers", R, [
        ('        "Left Output Mixer MIC1Booststage Switch", "Right Output Mixer MIC1Booststage Switch",\n', "")]),
    ("held batch: Line In stays in the output mixers", R, [
        ('        "Left Output Mixer MIC2Booststage Switch", "Right Output Mixer MIC2Booststage Switch",\n', "")]),
    ("held batch: the recorded mix stays (a test records the headphone mix)", R, [
        ('        "LADC input Mixer l_output mixer Switch",  "RADC input Mixer r_output mixer Switch",\n', "")]),
    ("hold: one release ends every holder's hold", M, [
        ("    held = hold ? (held | holder) : (held & ~(unsigned)holder);", "    held = hold ? (held | holder) : 0;")]),
    ("AIF1 DAC timeslot volumes not the mixer's (DVR Playback leaks into HDMI-in audio)", R, [
        ('    cset(&cb, "AIF1 DAC timeslot 0 volume", DIGITAL_VOLUME_OLD);\n    cset(&cb, "AIF1 DAC timeslot 1 volume", DIGITAL_VOLUME_OLD);\n', "")]),
    ("hold: the watcher keeps applying while held", M, [
        ("        while (held)\n            pthread_cond_wait(&work_cond, &mixer_mutex);\n        while (applied == requested && !held) {",
         "        while (applied == requested) {"),
        ("            if (applied != requested || held)\n                continue; // a new state came meanwhile, or ripples' code took the codec",
         "            if (applied != requested)\n                continue; // a new state came meanwhile"),
        ("        if (held)\n            continue;\n        if (ui_requests != seen_ui) {", "        if (ui_requests != seen_ui) {"),
        ("    if (held) {\n        // ripples' own code drives the codec", "    if (0) {\n        // ripples' own code drives the codec")]),
]


def run_test(src, targets):
    cmd = [sys.executable, "-I", os.path.join(HERE, "test_mixer.py"), "--src", src, "--targets", targets]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1200)
    return p.returncode, p.stdout


def main():
    targets = "HDZGOGGLE,HDZGOGGLE2,HDZBOXPRO"
    jobs = max(1, (os.cpu_count() or 2) // 2)
    args = sys.argv[1:]
    while args and args[0] in ("--targets", "--jobs"):
        if args[0] == "--targets":
            targets = args[1]
        else:
            jobs = int(args[1])
        args = args[2:]
    only = args[0] if args else None

    work = tempfile.mkdtemp(prefix="audio_mixer_mutants_")
    survived = 0
    try:
        clean = os.path.join(work, "clean")
        os.makedirs(os.path.join(clean, "core"))
        for f in FILES:
            shutil.copy(os.path.join(ROOT, "src/core", f), os.path.join(clean, "core", f))
        status, out = run_test(clean, targets)
        if status != 0:
            print("the unmodified sources fail:\n" + out)
            return 1
        print("unmodified: passes", flush=True)

        selected = []
        for i, (name, file, edits) in enumerate(MUTANTS):
            if only and only not in name:
                continue
            src = os.path.join(work, f"m{i}")
            shutil.copytree(clean, src)
            path = os.path.join(src, "core", file)
            text = open(path).read()
            for old, new in edits:
                if text.count(old) != 1:
                    print(f"mutant '{name}': cannot apply, '{old[:60]}' found {text.count(old)} times")
                    return 1
                text = text.replace(old, new)
            open(path, "w").write(text)
            selected.append((name, src))

        # each mutant in its own copy and its own test directory: in parallel
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            results = pool.map(lambda m: (m[0], run_test(m[1], targets)), selected)
            for name, (status, out) in results:
                first = next((line for line in out.splitlines() if line.startswith("FAIL")), "")
                if "build failed" in out:
                    print(f"mutant '{name}' does not build: {first[:150]}")
                    return 1
                if status == 0:
                    survived += 1
                    print(f"SURVIVED  {name}", flush=True)
                else:
                    print(f"killed    {name}: {first[:150]}", flush=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print(f"{len(selected)} mutants, {survived} survived")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
