#!/usr/bin/env python3
# Minimal RTSP client (RTP over the RTSP TCP connection) to look at what the server sends.
# usage: rtsp_rtp.py URL SECONDS [OUT_PREFIX]
#   prints the SDP and, per track: packets, access units (distinct RTP timestamps), RTP marker
#   placement, the NAL unit types of the packets that carry the marker, RTP timestamp steps;
#   with OUT_PREFIX writes OUT_PREFIX.sdp and OUT_PREFIX.pkts (one line per packet: track,
#   marker, payload size, first payload bytes, timestamp step) for comparisons.
import socket
import sys
import time
from collections import Counter
from urllib.parse import urlparse


class Rtsp:
    def __init__(self, url):
        u = urlparse(url)
        self.url = url
        self.s = socket.create_connection((u.hostname, u.port or 554), timeout=15)
        self.cseq = 1
        self.buf = b""
        self.session = None

    def request(self, method, url, headers=()):
        h = "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: rtsp_rtp\r\n" % (method, url, self.cseq)
        if self.session:
            h += "Session: %s\r\n" % self.session
        for x in headers:
            h += x + "\r\n"
        self.s.sendall((h + "\r\n").encode())
        self.cseq += 1
        while True:
            head, body = self.read_response()
            if head is not None:
                return head, body

    def fill(self):
        d = self.s.recv(65536)
        if not d:
            raise EOFError
        self.buf += d

    def read_response(self):
        while True:
            if self.buf[:1] == b"$":
                if self.read_interleaved() is None:
                    self.fill()
                continue
            i = self.buf.find(b"\r\n\r\n")
            if i >= 0:
                break
            self.fill()
        head = self.buf[:i].decode(errors="replace")
        self.buf = self.buf[i + 4:]
        length = 0
        for line in head.split("\r\n"):
            if line.lower().startswith("content-length:"):
                length = int(line.split(":")[1])
            if line.lower().startswith("session:"):
                self.session = line.split(":", 1)[1].strip().split(";")[0]
        while len(self.buf) < length:
            self.fill()
        body = self.buf[:length].decode(errors="replace")
        self.buf = self.buf[length:]
        return head, body

    def read_interleaved(self):
        if len(self.buf) < 4:
            return None
        n = int.from_bytes(self.buf[2:4], "big")
        if len(self.buf) < 4 + n:
            return None
        ch, pkt = self.buf[1], self.buf[4:4 + n]
        self.buf = self.buf[4 + n:]
        return ch, pkt


def main():
    url, seconds = sys.argv[1], float(sys.argv[2])
    out = sys.argv[3] if len(sys.argv) > 3 else None
    c = Rtsp(url)
    _, sdp = c.request("DESCRIBE", url, ["Accept: application/sdp"])
    tracks, media = [], None
    for line in sdp.splitlines():
        if line.startswith("m="):
            media = line.split()[0][2:]
        if line.startswith("a=control:") and media:
            ctl = line.split(":", 1)[1]
            tracks.append((media, ctl if ctl.startswith("rtsp:") else url.rstrip("/") + "/" + ctl))
            media = None
    for i, (_, turl) in enumerate(tracks):
        c.request("SETUP", turl, ["Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d" % (2 * i, 2 * i + 1)])
    c.request("PLAY", url, ["Range: npt=0.000-"])

    pkts = {i: [] for i in range(len(tracks))}
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        r = c.read_interleaved()
        if r is None:
            c.fill()
            continue
        ch, p = r
        if ch % 2 or len(p) < 12:
            continue
        cc = p[0] & 0x0F
        hl = 12 + 4 * cc
        if p[0] & 0x10:
            hl += 4 + 4 * int.from_bytes(p[hl + 2:hl + 4], "big")
        pkts[ch // 2].append((bool(p[1] & 0x80), int.from_bytes(p[4:8], "big"), p[hl:]))
    try:
        c.request("TEARDOWN", url)
    except (OSError, EOFError):
        pass

    print("SDP:")
    for line in sdp.splitlines():
        print("  " + line)
    lines = []
    for i, (media, _) in enumerate(tracks):
        ps = pkts[i]
        if not ps:
            print("%s: no packets" % media)
            continue
        h265 = "H265" in sdp.split("m=")[i + 1]
        steps = Counter()
        marked_types = Counter()
        aus, aus_marked_last, marks_inside = 0, 0, 0
        for k, (m, ts, pl) in enumerate(ps):
            nxt = ps[k + 1][1] if k + 1 < len(ps) else None
            last_of_au = nxt is not None and nxt != ts
            if k > 0:
                steps[(ts - ps[k - 1][1]) & 0xFFFFFFFF] += 1
            if media == "video" and pl:
                if h265:
                    t = (pl[0] >> 1) & 0x3F
                    if t == 49 and len(pl) > 2:
                        t = pl[2] & 0x3F  # FU: type of the fragmented unit
                else:
                    t = pl[0] & 0x1F
                    if t == 28 and len(pl) > 1:
                        t = pl[1] & 0x1F
                if m:
                    marked_types[t] += 1
            if nxt is not None:
                if last_of_au:
                    aus += 1
                    aus_marked_last += m
                elif m:
                    marks_inside += 1
            step = ((ts - ps[k - 1][1]) & 0xFFFFFFFF) if k > 0 else 0
            lines.append("%s %d %d %s %d" % (media, m, len(pl), pl[:3].hex(), step))
        common = ", ".join("%d x%d" % (s, n) for s, n in steps.most_common(6))
        print("%s: %d packets, %d complete access units, marker on the last packet of %d of them, "
              "%d markers before the end of an access unit" % (media, len(ps), aus, aus_marked_last, marks_inside))
        if media == "video":
            print("  NAL types carrying the marker: %s" % dict(sorted(marked_types.items())))
        print("  RTP timestamp steps (most common): %s" % common)
    if out:
        with open(out + ".sdp", "w") as f:
            f.write(sdp)
        with open(out + ".pkts", "w") as f:
            f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
