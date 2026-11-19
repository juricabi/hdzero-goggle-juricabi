#!/usr/bin/env python3
# Time RTSP DESCRIBE requests, each from its own connection (= its own client).
# usage: rtsp_describe.py URL START[,START...]   (start offsets in seconds, may overlap)
# prints one line per request: start offset, completion offset, duration, media in the SDP
import socket
import sys
import threading
import time
from urllib.parse import urlparse


def describe(url, timeout=15.0):
    u = urlparse(url)
    s = socket.create_connection((u.hostname, u.port or 554), timeout=timeout)
    s.sendall(("DESCRIBE %s RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\nUser-Agent: rtsp_describe\r\n\r\n" % url).encode())
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    head, _, body = data.partition(b"\r\n\r\n")
    length = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":")[1])
    while len(body) < length:
        chunk = s.recv(4096)
        if not chunk:
            break
        body += chunk
    s.close()
    status = head.split(b"\r\n")[0].decode(errors="replace")
    media = [l.split()[0][2:] for l in body.decode(errors="replace").splitlines() if l.startswith("m=")]
    return status, media


def main():
    url = sys.argv[1]
    starts = [float(x) for x in sys.argv[2].split(",")] if len(sys.argv) > 2 else [0.0]
    t0 = time.monotonic()
    results = [None] * len(starts)

    def run(i):
        delay = t0 + starts[i] - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        b = time.monotonic()
        try:
            status, media = describe(url)
        except OSError as e:
            status, media = "error: %s" % e, []
        e = time.monotonic()
        results[i] = (b - t0, e - t0, e - b, status, media)

    threads = [threading.Thread(target=run, args=(i,)) for i in range(len(starts))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for i, (b, e, d, status, media) in enumerate(results):
        print("client %d: start +%.3f s, done +%.3f s, DESCRIBE %.3f s, %s, m=%s" % (i, b, e, d, status, ",".join(media) or "-"))


if __name__ == "__main__":
    main()
