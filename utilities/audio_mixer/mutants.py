#!/usr/bin/env python3
"""Mutation check of test_mixer.py in juricabi's firmware: every mutant of the mixer sources must make it fail.

    utilities/audio_mixer/mutants.py [--targets HDZGOGGLE2,...] [name filter]

Each mutant is one small change to a copy of src/core/audio_mixer*.c; test_mixer.py runs against
the copy (--src). all's mutants (082b2f5) that still apply, the ones of all's reviews among them,
and the ones of juricabi's own rules (ripples' controls, the settings both ways, hold), the fixes of
the review of phase 3 (the held batch, holders, the timeslot volumes, the round trip through stock
ripples), juricabi.2's page (no linked rows, Record Level, the ADC keeping a source, the take-over
of juricabi.1's Record Headphone Mix), and juricabi.4's watcher (the codec read only while something
may have changed it). Exit status 1 if a mutant survives (or the unmodified copy fails).
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
    ("no watcher", M, [("const int has = codec_has_last_batch();", "const int has = codec_has_last_batch() >= 0;")]),
    ("watcher: no backoff after the retries", M, [("#define AUDIO_MIXER_WATCH_RETRIES 3", "#define AUDIO_MIXER_WATCH_RETRIES 1000")]),
    ("review x3: ADC volume never written", R, [('    cset(&cb, "ADC volume", adc_volume);\n', "    (void)adc_volume;\n")]),
    ("review x4: HDMI level is not a headphone level", M, [
        ("field == AUDIO_MIXER_AV || field == AUDIO_MIXER_HDMI;", "field == AUDIO_MIXER_AV;")]),
    ("review x5: unused output-mixer gains not reset to 0 dB", R, [
        ('    if (!out_mic)\n        cset(&cb, "MIC1_G boost stage output mixer control", OUTPUT_GAIN_0DB);\n', "")]),
    ("review x7: set() does not clamp", M, [
        ("    mixer.value[field] = clamp_field(field, value);\n    if (is_headphone_level",
         "    mixer.value[field] = (uint8_t)value;\n    if (is_headphone_level")]),
    ("line out off: sources stay in the output mixers", R, [
        ("const bool connect = line_out;", "const bool connect = true;")]),
    ("line out off: the DAC stays in the output mixers", R, [
        ("const int out_dac = connect && dac_on;", "const int out_dac = dac_on;")]),
    ("HDMI in: a level change switches the line out on", M, [
        ("        if (!line_out_by_source)\n            context.line_out = true;", "        context.line_out = true;")]),
    # ---- juricabi.4: the watcher reads the codec only while something may have changed it (the
    # 250 ms watcher of juricabi.2 / .3 clicked in every DVR recording)
    ("watcher: reads the codec all the time (the clicks of juricabi.2 / .3)", M, [
        ("                if (!recheck_now && !unverified && !backoff && mono_ms() >= recheck_until_ms)\n"
         "                    continue; // nothing may have changed the codec: leave it alone\n", "")]),
    ("watcher: keeps reading after the codec has the batch", M, [
        ("                backoff = 0;\n                unverified = false;\n", "                backoff = 0;\n")]),
    ("watcher: an apply not checked", M, [("        apply_latest();\n        unverified = true;\n", "        apply_latest();\n")]),
    ("watcher: a record daemon start not noticed", M, [
        ("    if (pid <= 0 || pid == *seen)\n", "    if (1 || pid <= 0 || pid == *seen)\n")]),
    ("watcher: the daemon running at boot taken as already looked at", M, [
        ("    int daemon = 0;          // the record daemon's pid at the last look (0: none seen yet)\n",
         "    int daemon = audio_mixer_record_daemon_pid();\n")]),
    ("watcher: a pass reads only part of the batch", M, [
        ("        codec_reads++;\n", "        if (strstr(name, \"MIC2\"))\n            continue;\n        codec_reads++;\n")]),
    ("watcher: recheck ignored", M, [("    recheck_now = true;\n    pthread_cond_signal(&work_cond);",
                                      "    pthread_cond_signal(&work_cond);")]),
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
    ("Mic Gain on the MIC2 preamp", R, [
        ('cset(&cb, "MIC1 boost AMP gain control", v[AUDIO_MIXER_MIC_GAIN]);', 'cset(&cb, "MIC1 boost AMP gain control", v[AUDIO_MIXER_LINEIN_GAIN]);')]),
    ("Line/AV Gain not on the MIC2 preamp", R, [
        ('cset(&cb, "MIC2 boost AMP gain control", v[AUDIO_MIXER_LINEIN_GAIN]);', 'cset(&cb, "MIC2 boost AMP gain control", 4);')]),
    ("defaults: Record Level 0 dB with Line In / A/V In (not ripples' Line/AV Gain)", R, [
        ("v[AUDIO_MIXER_REC_LEVEL] = dvr_source == 0 ? OUTPUT_GAIN_0DB : clamp(rec->linein_gain, AUDIO_MIXER_GAIN_MAX);",
         "v[AUDIO_MIXER_REC_LEVEL] = OUTPUT_GAIN_0DB;")]),
    ("defaults: Record Level Line/AV Gain with the mic too", R, [
        ("v[AUDIO_MIXER_REC_LEVEL] = dvr_source == 0 ? OUTPUT_GAIN_0DB : clamp(rec->linein_gain, AUDIO_MIXER_GAIN_MAX);",
         "v[AUDIO_MIXER_REC_LEVEL] = clamp(rec->linein_gain, AUDIO_MIXER_GAIN_MAX);")]),
    ("defaults: Mic Gain not ripples'", R, [
        ("v[AUDIO_MIXER_MIC_GAIN] = clamp(rec->mic_gain, AUDIO_MIXER_GAIN_MAX);", "v[AUDIO_MIXER_MIC_GAIN] = 4;")]),

    # ---- juricabi.2's page: each row on its own
    ("ADC gain: Line In Boost, not Record Level", R, [
        ("const int adc_gain = v[AUDIO_MIXER_REC_LEVEL];", "const int adc_gain = v[AUDIO_MIXER_LINEIN_GAIN];")]),
    ("ADC gain: 0 dB whatever Record Level says", R, [
        ("const int adc_gain = v[AUDIO_MIXER_REC_LEVEL];", "const int adc_gain = OUTPUT_GAIN_0DB;")]),
    ("nothing recorded: the ADC without a source", R, [("const int adc_mic = rec_mic || silent;", "const int adc_mic = rec_mic;")]),
    ("nothing recorded: the mic recorded at full volume", R, [
        ("const int adc_volume = silent ? ADC_VOLUME_SILENT : DIGITAL_VOLUME_OLD;", "const int adc_volume = DIGITAL_VOLUME_OLD;")]),
    ("recorded: the ADC at its minimum volume", R, [
        ("const int adc_volume = silent ? ADC_VOLUME_SILENT : DIGITAL_VOLUME_OLD;", "const int adc_volume = ADC_VOLUME_SILENT;")]),
    ("a heard input is recorded through the output mixers", R, [
        ('{"LADC input Mixer l_output mixer Switch", 0},', '{"LADC input Mixer l_output mixer Switch", out_mic > 0},')]),
    ("a heard input is not recorded directly", R, [("const int adc_line = v[AUDIO_MIXER_REC_LINE_IN] != 0;",
                                                     "const int adc_line = v[AUDIO_MIXER_REC_LINE_IN] != 0 && mon_line == 0;")]),
    ("Headphone Volume moves A/V In again", M, [
        ("    if (mixer.value[field] != old)\n        follow_locked(field);",
         "    if (mixer.value[field] != old)\n        follow_locked(field);\n    if (field == AUDIO_MIXER_LIVE_VOLUME)\n"
         "        mixer.value[AUDIO_MIXER_AV] = audio_mixer_live_av_level(mixer.value[AUDIO_MIXER_LIVE_VOLUME]);")]),
    ("Audio Source does not follow the Record switches", M, [
        ("        mixer.value[AUDIO_MIXER_SOURCE] = audio_mixer_source_of(&mixer);\n    if (field == AUDIO_MIXER_AV",
         "        (void)0;\n    if (field == AUDIO_MIXER_AV")]),
    ("Audio Source: the last recorded input, not the first", R, [
        ("    for (int i = 0; i < 3; i++)\n        if (settings->value[rec[i]])", "    for (int i = 2; i >= 0; i--)\n        if (settings->value[rec[i]])")]),
    ("Audio Source: moves away from a recorded current one", R, [
        ("    if (settings->value[rec[current]])\n        return current;\n", "")]),
    ("receiver audio does not follow A/V In", M, [
        ("        mixer.value[AUDIO_MIXER_RX_AUDIO] = mixer.value[AUDIO_MIXER_AV] > 0 || mixer.value[AUDIO_MIXER_REC_AV];",
         "        (void)0;")]),
    ("receiver audio follows only the A/V In level", M, [
        ("        mixer.value[AUDIO_MIXER_RX_AUDIO] = mixer.value[AUDIO_MIXER_AV] > 0 || mixer.value[AUDIO_MIXER_REC_AV];",
         "        mixer.value[AUDIO_MIXER_RX_AUDIO] = mixer.value[AUDIO_MIXER_AV] > 0;")]),
    ("juricabi.1: Record Headphone Mix ignored", M, [("        if (rec_mix > 0) {", "        if (rec_mix > 99) {")]),
    ("juricabi.1: the mix records an input heard at level 0", M, [
        ("                if (mixer.value[audio_mixer_level_of(f)] > 0 && !mixer.value[f]) {",
         "                if (!mixer.value[f]) {")]),
    ("juricabi.1: the taken-over switches not written with the next change", M, [
        ("                    mixer.value[f] = 1;\n                    stale |= FIELD_BIT(f);", "                    mixer.value[f] = 1;")]),
    ("juricabi.1: rec_mix left in setting.ini", M, [("        ini_puts(AUDIO_MIXER_SECTION, \"rec_mix\", NULL, SETTING_INI);\n", "")]),
    ("juricabi.1: Record Level 0 dB whatever was recorded", M, [
        ("            mixer.value[AUDIO_MIXER_REC_LEVEL] = more_than_mic ? clamp_field(AUDIO_MIXER_REC_LEVEL, rec->linein_gain) : 3;",
         "            (void)more_than_mic;\n            mixer.value[AUDIO_MIXER_REC_LEVEL] = 3;")]),
    ("juricabi.1: Record Level ignores the mix", M, [
        ("            const bool more_than_mic = rec_mix > 0 || mixer.value[AUDIO_MIXER_REC_LINE_IN] || mixer.value[AUDIO_MIXER_REC_AV];",
         "            const bool more_than_mic = mixer.value[AUDIO_MIXER_REC_LINE_IN] || mixer.value[AUDIO_MIXER_REC_AV];")]),
    ("Record Level read from setting.ini over a computed one", M, [
        ("        if (!(in_file & FIELD_BIT(AUDIO_MIXER_REC_LEVEL))) {\n            const bool more_than_mic",
         "        if (0) {\n            const bool more_than_mic")]),

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
    ("all's firmware: Line In Boost from its record level", M, [
        ("ini_getl(AUDIO_MIXER_SECTION, \"line_in_boost\", 4, SETTING_INI)", "ini_getl(AUDIO_MIXER_SECTION, \"dvr_level\", 4, SETTING_INI)")]),
    ("all's firmware: Record Level not from its record level", M, [
        ("        if (!(in_file & FIELD_BIT(AUDIO_MIXER_REC_LEVEL)) && has_key(AUDIO_MIXER_SECTION, \"dvr_level\"))",
         "        if (0)")]),
    ("all's firmware: DVR Playback not from its playback volume", M, [
        ("            rec->dvr_volume = dvr_volume_of_playback(", "            (void)dvr_volume_of_playback(")]),
    ("all's firmware: playback mapped to the nearest level below", M, [
        ("        if (line_out[level] >= playback)\n            return level;", "        if (line_out[level] > playback)\n            return level - 1;")]),
    ("save: ripples' keys written without a change", M, [
        ("        if (current.value[i] != saved.value[i] || (stale & FIELD_BIT(i))) {",
         "        if (current.value[i] != saved.value[i] || (stale & FIELD_BIT(i)) || strcmp(field_ini[i].section, RECORD_SECTION) == 0) {")]),
    # ---- the round trip juricabi -> stock ripples -> juricabi (review of phase 3, finding 2)
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
    ("held batch: the output mixers stay on the ADC", R, [
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
