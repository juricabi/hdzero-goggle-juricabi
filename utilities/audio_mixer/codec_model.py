"""Model of the V536 (sun8iw16) internal codec's mixer, and of the old firmware's audio routing.

Signal graph: the DAPM routes of the Allwinner driver (sound/soc/sunxi/sun8iw16-codec.c,
ac_dapm_routes[]), the part between the analog inputs / AIF1 playback and the AIF1 capture /
line out. A route (sink, control, source) is connected when its control is: a mixer switch
"<sink> <control>" = 1, a mux "<sink>" whose current item is <control>, or always (None).
The gains that act on a path are the controls of the widgets and edges it goes through.

evaluate() answers, for a codec state: which sources reach the recording (AIF1 capture left /
right) and the headphones (line out left / right), and through which gain settings.
"""

import re

# ac_dapm_routes[] of sun8iw16-codec.c (the relevant part, verbatim). Note the driver's pin
# names: the left output mixer and the right ADC take LINEINP, the right output mixer and the
# left ADC take LINEINN. Which pin is which channel does not matter here: both are A/V.
ROUTES = [
    ("AIF1ADCL", None, "AIF1OUT0L Mux"),
    ("AIF1ADCR", None, "AIF1OUT0R Mux"),
    ("AIF1OUT0L Mux", "AIF1_AD0L", "AIF1 AD0L Mixer"),
    ("AIF1OUT0L Mux", "AIF1_AD0R", "AIF1 AD0R Mixer"),
    ("AIF1OUT0R Mux", "AIF1_AD0R", "AIF1 AD0R Mixer"),
    ("AIF1OUT0R Mux", "AIF1_AD0L", "AIF1 AD0L Mixer"),
    ("AIF1 AD0L Mixer", "AIF1 DA0L Switch", "AIF1IN0L Mux"),
    ("AIF1 AD0L Mixer", "ADCL Switch", "LADC input Mixer"),
    ("AIF1 AD0R Mixer", "AIF1 DA0R Switch", "AIF1IN0R Mux"),
    ("AIF1 AD0R Mixer", "ADCR Switch", "RADC input Mixer"),
    ("AIF1IN0L Mux", "AIF1_DA0L", "AIF1DACL"),
    ("AIF1IN0L Mux", "AIF1_DA0R", "AIF1DACR"),
    ("AIF1IN0R Mux", "AIF1_DA0R", "AIF1DACR"),
    ("AIF1IN0R Mux", "AIF1_DA0L", "AIF1DACL"),
    ("DACL Mixer", "AIF1DA0L Switch", "AIF1IN0L Mux"),
    ("DACL Mixer", "ADCL Switch", "LADC input Mixer"),
    ("DACR Mixer", "AIF1DA0R Switch", "AIF1IN0R Mux"),
    ("DACR Mixer", "ADCR Switch", "RADC input Mixer"),
    ("Right Output Mixer", "DACR Switch", "DACR Mixer"),
    ("Right Output Mixer", "DACL Switch", "DACL Mixer"),
    ("Right Output Mixer", "LINEINR Switch", "LINEINN"),
    ("Right Output Mixer", "MIC2Booststage Switch", "MIC2 PGA"),
    ("Right Output Mixer", "MIC1Booststage Switch", "MIC1 PGA"),
    ("Left Output Mixer", "DACL Switch", "DACL Mixer"),
    ("Left Output Mixer", "DACR Switch", "DACR Mixer"),
    ("Left Output Mixer", "LINEINL Switch", "LINEINP"),
    ("Left Output Mixer", "MIC2Booststage Switch", "MIC2 PGA"),
    ("Left Output Mixer", "MIC1Booststage Switch", "MIC1 PGA"),
    ("LINEOUTL Mux", "LOMIX", "Left Output Mixer"),
    ("LINEOUTL Mux", "OMIX_LR_SUM", "Left Output Mixer"),
    ("LINEOUTL Mux", "OMIX_LR_SUM", "Right Output Mixer"),
    ("LINEOUTR Mux", "ROMIX", "Right Output Mixer"),
    ("LINEOUTR Mux", "OMIX_LR_SUM", "Left Output Mixer"),
    ("LINEOUTR Mux", "OMIX_LR_SUM", "Right Output Mixer"),
    ("LINEOUTL", None, "LINEOUTL Mux"),
    ("LINEOUTR", None, "LINEOUTR Mux"),
    ("LADC input Mixer", "MIC1 boost Switch", "MIC1 PGA"),
    ("LADC input Mixer", "MIC2 boost Switch", "MIC2 PGA"),
    ("LADC input Mixer", "LINEINL Switch", "LINEINN"),
    ("LADC input Mixer", "l_output mixer Switch", "Left Output Mixer"),
    ("LADC input Mixer", "r_output mixer Switch", "Right Output Mixer"),
    ("RADC input Mixer", "MIC1 boost Switch", "MIC1 PGA"),
    ("RADC input Mixer", "MIC2 boost Switch", "MIC2 PGA"),
    ("RADC input Mixer", "LINEINR Switch", "LINEINP"),
    ("RADC input Mixer", "r_output mixer Switch", "Right Output Mixer"),
    ("RADC input Mixer", "l_output mixer Switch", "Left Output Mixer"),
    ("MIC1 PGA", None, "MIC1P"),
    ("MIC1 PGA", None, "MIC1N"),
    ("MIC2 PGA", None, "MIC2P"),
    ("MIC2 PGA", None, "MIC2N"),
]

# enum texts of the muxes (left_lineout_text[], aif1out0l_text[], ...)
MUX_ITEMS = {
    "LINEOUTL Mux": ["LOMIX", "OMIX_LR_SUM"],
    "LINEOUTR Mux": ["ROMIX", "OMIX_LR_SUM"],
    "AIF1OUT0L Mux": ["AIF1_AD0L", "AIF1_AD0R", "SUM_AIF1AD0L_AIF1AD0R", "AVE_AIF1AD0L_AIF1AD0R"],
    "AIF1OUT0R Mux": ["AIF1_AD0R", "AIF1_AD0L", "SUM_AIF1AD0L_AIF1AD0R", "AVE_AIF1AD0L_AIF1AD0R"],
    "AIF1IN0L Mux": ["AIF1_DA0L", "AIF1_DA0R", "SUM_AIF1DA0L_AIF1DA0R", "AVE_AIF1DA0L_AIF1DA0R"],
    "AIF1IN0R Mux": ["AIF1_DA0R", "AIF1_DA0L", "SUM_AIF1DA0L_AIF1DA0R", "AVE_AIF1DA0L_AIF1DA0R"],
}

# input endpoints -> source (what the goggle connects there)
SOURCES = {
    "MIC1P": "mic", "MIC1N": "mic",          # built-in microphone
    "MIC2P": "line_in", "MIC2N": "line_in",  # "Line In" jack
    "LINEINP": "av", "LINEINN": "av",        # AV-in jack, module bay, analog receiver
    "AIF1DACL": "dac", "AIF1DACR": "dac",    # DVR playback (active only while a file plays)
}
ANALOG_INPUTS = {"MIC1P", "MIC1N", "MIC2P", "MIC2N", "LINEINP", "LINEINN"}
DAC_INPUTS = {"AIF1DACL", "AIF1DACR"}

# gains of a widget (applied to everything that leaves it)
NODE_GAINS = {
    "MIC1 PGA": ["MIC1 boost AMP gain control"],
    "MIC2 PGA": ["MIC2 boost AMP gain control"],
    "LADC input Mixer": ["ADC input gain control", "ADC volume"],  # analog ADC gain, digital ADC volume
    "RADC input Mixer": ["ADC input gain control", "ADC volume"],
    "DACL Mixer": ["DAC volume"],
    "DACR Mixer": ["DAC volume"],
}
# gains of an edge (sink, source)
EDGE_GAINS = {}
for side in ("Left", "Right"):
    EDGE_GAINS[(f"{side} Output Mixer", "MIC1 PGA")] = "MIC1_G boost stage output mixer control"
    EDGE_GAINS[(f"{side} Output Mixer", "MIC2 PGA")] = "MIC2 BST stage to L_R outp mixer gain"
EDGE_GAINS[("Left Output Mixer", "LINEINP")] = "LINEINL/R to L_R output mixer gain"
EDGE_GAINS[("Right Output Mixer", "LINEINN")] = "LINEINL/R to L_R output mixer gain"
EDGE_GAINS[("LINEOUTL", "LINEOUTL Mux")] = "lineout volume"
EDGE_GAINS[("LINEOUTR", "LINEOUTR Mux")] = "lineout volume"
OUTPUT_MIXER_SIDE = {"Left Output Mixer": "L", "Right Output Mixer": "R"}

BY_SINK = {}
for _sink, _control, _source in ROUTES:
    BY_SINK.setdefault(_sink, []).append((_control, _source))

LINE_OUT_PIN = "Lineout Switch"  # SOC_DAPM_PIN_SWITCH("Lineout") of sun8iw16-sndcodec.c


def control_name(sink, control):
    return f"{sink} {control}"


def edge_connected(state, sink, control):
    if control is None:
        return True
    if sink in MUX_ITEMS:
        items = MUX_ITEMS[sink]
        index = state.get(sink, 0)
        return 0 <= index < len(items) and items[index] == control
    return state.get(control_name(sink, control), 0) == 1


def paths(state, sink, active):
    """Every path from an active input endpoint to `sink`: (pin, frozenset of gains, mixer side)."""
    found = []

    def walk(node, visited, gains, side):
        if node in SOURCES:
            if node in active:
                found.append((node, frozenset(gains), side))
            return
        for control, source in BY_SINK.get(node, []):
            if source in visited or not edge_connected(state, node, control):
                continue
            g = list(gains)
            edge_gain = EDGE_GAINS.get((node, source))
            if edge_gain:
                g.append((edge_gain, state.get(edge_gain)))
            for node_gain in NODE_GAINS.get(source, []):
                g.append((node_gain, state.get(node_gain)))
            walk(source, visited | {source}, g, OUTPUT_MIXER_SIDE.get(source, side))

    walk(sink, {sink}, [], None)
    return found


def summarize(found):
    """{source: {(gains, output mixer side)}}: what reaches a sink, and how."""
    out = {}
    for pin, gains, side in found:
        out.setdefault(SOURCES[pin], set()).add((gains, side))
    return out


def evaluate(state, dac_active=False):
    """What the recording and the headphones get in a codec state."""
    active = set(ANALOG_INPUTS) | (DAC_INPUTS if dac_active else set())
    result = {
        "record_l": summarize(paths(state, "AIF1ADCL", active)),
        "record_r": summarize(paths(state, "AIF1ADCR", active)),
    }
    line_out = state.get(LINE_OUT_PIN, 0) == 1
    result["phones_l"] = summarize(paths(state, "LINEOUTL", active)) if line_out else {}
    result["phones_r"] = summarize(paths(state, "LINEOUTR", active)) if line_out else {}
    return result


def line_out_powered(state, dac_active=False):
    """DAPM powers the line out (and the PA) when the pin is on and an input reaches it."""
    active = set(ANALOG_INPUTS) | (DAC_INPUTS if dac_active else set())
    return state.get(LINE_OUT_PIN, 0) == 1 and bool(paths(state, "LINEOUTL", active) or paths(state, "LINEOUTR", active))


def capture_connected(state, dac_active=False):
    active = set(ANALOG_INPUTS) | (DAC_INPUTS if dac_active else set())
    return bool(paths(state, "AIF1ADCL", active) or paths(state, "AIF1ADCR", active))


def describe(result):
    lines = []
    for sink in ("record_l", "record_r", "phones_l", "phones_r"):
        parts = []
        for source in sorted(result[sink]):
            for gains, side in sorted(result[sink][source], key=lambda x: (sorted(x[0]), str(x[1]))):
                g = ", ".join(f"{k}={v}" for k, v in sorted(gains))
                parts.append(f"{source}{'/' + side if side else ''} [{g}]")
        lines.append(f"  {sink}: {'; '.join(parts) if parts else '-'}")
    return "\n".join(lines)


# ---------------------------------------------------------------------------------------------
# amixer: the device's control list, and batches

def parse_amixer_contents(path):
    """Controls of an `amixer contents` dump: name -> {type, min, max, items, count, value}."""
    controls = {}
    current = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"numid=\d+,iface=MIXER,name='(.*)'", line)
        if m:
            current = {"name": m.group(1)}
            controls[current["name"]] = current
            continue
        if current is None:
            continue
        m = re.search(r"type=(\w+),.*values=(\d+)", line)
        if m:
            current["type"], current["count"] = m.group(1), int(m.group(2))
            mm = re.search(r"min=(-?\d+),max=(-?\d+)", line)
            if mm:
                current["min"], current["max"] = int(mm.group(1)), int(mm.group(2))
            mi = re.search(r"items=(\d+)", line)
            if mi:
                current["min"], current["max"] = 0, int(mi.group(1)) - 1
            if current["type"] == "BOOLEAN":
                current["min"], current["max"] = 0, 1
        m = re.match(r"\s+: values=(.*)", line)
        if m:
            first = m.group(1).split(",")[0]
            current["value"] = {"on": 1, "off": 0}.get(first, None) if first in ("on", "off") else int(first)
    return controls


def parse_batch(text):
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        m = re.fullmatch(r"cset name='([^']+)' (-?\d+)", line)
        if not m:
            raise ValueError(f"not an amixer -s cset line: {line!r}")
        out.append((m.group(1), int(m.group(2))))
    return out


def check_batch(controls, batch):
    """Every control exists and gets a value in its range, once."""
    errors = []
    seen = set()
    for name, value in batch:
        ctl = controls.get(name)
        if ctl is None:
            errors.append(f"unknown control '{name}'")
        elif not ctl["min"] <= value <= ctl["max"]:
            errors.append(f"'{name}' = {value} outside {ctl['min']}..{ctl['max']}")
        if name in seen:
            errors.append(f"'{name}' set twice")
        seen.add(name)
    return errors


# ---------------------------------------------------------------------------------------------
# The old firmware (before the Audio page): driver defaults, libmedia_mpp's reset, audio_sel.sh

# What the codec driver sets at probe from the device tree (sys_config.fex [codec]), per target.
# Official images: Goggle Rev20250319, Goggle 2 Rev20250715, BoxPro Rev20250517.
DEVICE_TREE = {
    "HDZGOGGLE": {"lineout volume": 31, "MIC1 boost AMP gain control": 4, "MIC2 boost AMP gain control": 4},
    "HDZGOGGLE2": {"lineout volume": 31, "MIC1 boost AMP gain control": 4, "MIC2 boost AMP gain control": 4},
    "HDZBOXPRO": {"lineout volume": 31, "MIC1 boost AMP gain control": 4, "MIC2 boost AMP gain control": 4},
}

# libmedia_mpp audioHw_Construct() -> alsaOpenMixer(), run by AW_MPI_SYS_Init() (record daemon
# start, DVR playback in the app).
LIBMEDIA_RESET = [
    ("Lineout Switch", 1),
    ("DACL Mixer AIF1DA0L Switch", 1),
    ("Left Output Mixer DACL Switch", 1),
    ("AIF1 AD0L Mixer ADCL Switch", 1),
    ("LADC input Mixer MIC1 boost Switch", 1),
    ("lineout volume", 27),
    ("DAC volume", 160),
    ("LINEOUTL Mux", 1),
    ("LINEOUTR Mux", 1),
    ("MIC1 boost AMP gain control", 4),
    ("ADC input gain control", 3),
    ("ADC volume", 160),
]


def daemon_restart(state):
    """The codec after the record daemon (re)starts, or DVR playback opens the player: libmedia_mpp's
    AW_MPI_SYS_Init() writes LIBMEDIA_RESET over whatever the mixer had set (line out on at 27)."""
    return apply(state, LIBMEDIA_RESET)


def heard(state, dac_active=False):
    """Sources that reach the headphones (line out pin on, a path from an active input)."""
    result = evaluate(state, dac_active)
    return set(result["phones_l"]) | set(result["phones_r"])


def recorded_via_mix(state, dac_active=False):
    """Sources that reach the recording through an output mixer (Record Headphone Mix)."""
    result = evaluate(state, dac_active)
    out = set()
    for sink in ("record_l", "record_r"):
        for source, routes in result[sink].items():
            if any(side in ("L", "R") for _, side in routes):
                out.add(source)
    return out


def driver_defaults(controls, target):
    """Codec state after boot, before any user space: register reset values (switches off, muxes
    at item 0, the -4.5..+6 dB gains at 3, digital volumes 160: as in the dumps for everything
    nobody sets), the line out pin disabled by the machine driver, and the device tree values."""
    state = {}
    for name, ctl in controls.items():
        if ctl["type"] in ("BOOLEAN", "ENUMERATED"):
            state[name] = 0
        else:
            state[name] = ctl.get("value", 0)
    state.update(DEVICE_TREE[target])
    return state


class AudioSel:
    """The old mkapp/app/script/audio_sel.sh, read from the repository."""

    def __init__(self, path):
        self.src = open(path, encoding="utf-8").read()
        self.funcs = {m.group(1): m.group(2) for m in re.finditer(r"function (\w+)\(\)\s*\{(.*?)\n\}", self.src, re.S)}

    def _func(self, name, arg):
        out = []
        for line in self.funcs[name].splitlines():
            line = line.strip()
            m = re.match(r"amixer cset name='([^']+)' (\S+)", line)
            if m:
                value = m.group(2)
                out.append((m.group(1), int(arg if value == "$onoff" else value)))
        return out

    def __call__(self, command):
        block = re.search(r'if \[ \$1 == "%s" \]\nthen\n(.*?)\nfi' % re.escape(command), self.src, re.S).group(1)
        out = []
        for line in block.splitlines():
            line = line.strip()
            m = re.match(r"amixer cset name='([^']+)' (\S+)", line)
            if m:
                out.append((m.group(1), int(m.group(2))))
                continue
            words = line.split()
            if words and words[0] in self.funcs:
                out += self._func(words[0], words[1] if len(words) > 1 else None)
        return out


def old_source_command(audio_source):
    """dvr_select_audio_source(uint8_t): the old [record] audio_source -> audio_sel.sh."""
    sel = audio_source & 0xFF
    return ["in_mic1", "in_mic2", "in_linein"][min(sel, 2)]


def apply(state, batch):
    state = dict(state)
    for name, value in batch:
        state[name] = value
    return state
