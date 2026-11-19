#!/usr/bin/env python3
# A/V offset of the test media markers (gen_media.sh) in a file: white video frames (flashes)
# against 1 kHz beep onsets.
# usage: avsync.py FILE [SKIP_SECONDS]   -> "n=<pairs> mean=<ms> min=<ms> max=<ms>" (beep - flash)
# (the beep of the first frame after SKIP_SECONDS on; ffmpeg aligns the tracks by RTCP sender
#  reports only after the first one arrived, so the start of a capture is skipped)
import re
import subprocess
import sys


def flashes(path):
    out = subprocess.run(["ffmpeg", "-hide_banner", "-nostats", "-i", path, "-map", "0:v:0", "-vf",
                          "signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=-", "-f", "null", "-"],
                         capture_output=True, text=True).stdout
    t, last, res = None, -1.0, []
    for line in out.splitlines():
        m = re.search(r"pts_time:([0-9.]+)", line)
        if m:
            t = float(m.group(1))
        m = re.search(r"YAVG=([0-9.]+)", line)
        if m and t is not None and float(m.group(1)) > 128 and t - last > 0.3:
            res.append(t)
            last = t
    return res


def beeps(path):
    err = subprocess.run(["ffmpeg", "-hide_banner", "-nostats", "-i", path, "-map", "0:a:0", "-af",
                          "silencedetect=n=-30dB:d=0.2", "-f", "null", "-"], capture_output=True, text=True).stderr
    return [float(x) for x in re.findall(r"silence_end: ([0-9.]+)", err)]


def main():
    path = sys.argv[1]
    skip = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
    v, a = flashes(path), beeps(path)
    offs = []
    for t in v:
        if t < (v[0] if v else 0) + skip or not a:
            continue
        b = min(a, key=lambda x: abs(x - t))
        if abs(b - t) < 0.25:
            offs.append(1000 * (b - t))
    if offs:
        print("n=%d mean=%.1f min=%.1f max=%.1f" % (len(offs), sum(offs) / len(offs), min(offs), max(offs)))
    else:
        print("n=0 (flashes %d, beeps %d)" % (len(v), len(a)))


if __name__ == "__main__":
    main()
