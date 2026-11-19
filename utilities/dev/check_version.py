#!/usr/bin/env python3
"""The firmware's version and image names (juricabi's firmware, built on ripples' 1.2.2):

    utilities/dev/check_version.py [--images <dir>]

- VERSION is "<ripples' version>-juricabi.<n>";
- the Firmware page (src/ui/page_version.c format_app_version(), compiled here on its own) shows it as
  "juricabi.<n> ripples <ripples' version>" in at most 31 characters, a development build's "-<commit>" cut
  to what fits; ripples' version still parses as the three numbers elrs.c sends to the backpack;
- the release workflows start on a tag "v<VERSION>";
- with --images: the three images a build at that tag makes, HDZERO_<T>-<rx>-<va>-<VERSION>.bin (no commit
  suffix), each with its .sha256, each a tar of hdzgoggle_app_ota-<VERSION>.tar and the target's RX / VA images.
"""
import hashlib
import os
import re
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
fails = 0


def check(ok, what):
    global fails
    print(("ok   " if ok else "FAIL ") + what)
    fails += not ok


version = open(os.path.join(ROOT, "VERSION")).read().strip()
m = re.fullmatch(r"(\d+\.\d+\.\d+)-juricabi\.(\d+)", version)
check(m is not None, f"VERSION {version!r} is <ripples' version>-juricabi.<n>")
base, n = (m.group(1), m.group(2)) if m else ("?", "?")

# the Firmware page's display, compiled on its own
src = open(os.path.join(ROOT, "src/ui/page_version.c"), encoding="utf-8").read()
macros = "\n".join(re.findall(r"^#define APP_VERSION_\w+ .*$", src, re.M))
start = src.index("static void format_app_version(")
end = src.index("\n}\n", start) + 3
cases = [(version, f"juricabi.{n} ripples {base}"),
         (version + "-abc1234", f"juricabi.{n} ripples {base}-abc1234"[:31]),
         ("1.2.2-juricabi.12", "juricabi.12 ripples 1.2.2"),
         ("1.2.2-ripples", "1.2.2-ripples")]
code = "#include <ctype.h>\n#include <stdio.h>\n#include <string.h>\n" + macros + "\n" + src[start:end] + r"""
int main(int argc, char **argv) {
    char out[32];
    unsigned char a, b, c;
    for (int i = 1; i < argc; i++) {
        format_app_version(argv[i], out, sizeof(out));
        const char *numeric = argv[i];
        while (*numeric && !isdigit((unsigned char)*numeric)) numeric++;
        int k = sscanf(numeric, "%hhu.%hhu.%hhu", &a, &b, &c);
        printf("%s|%d %u.%u.%u\n", out, k, a, b, c);
    }
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="check-version-") as d:
    c, exe = os.path.join(d, "v.c"), os.path.join(d, "v")
    open(c, "w").write(code)
    subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-Wno-format-truncation", "-o", exe, c], check=True)
    out = subprocess.run([exe] + [v for v, _ in cases], capture_output=True, text=True, check=True).stdout.splitlines()
for (v, want), line in zip(cases, out):
    shown, parsed = line.split("|")
    check(shown == want and len(shown) <= 31, f"Firmware page: {v!r} shown as {shown!r} (want {want!r}, at most 31 characters)")
    if "-juricabi." in v:
        check(parsed == "3 " + v.split("-")[0], f"{v!r}: ripples' version parsed as {parsed.split()[1]} (elrs.c sends it)")

# the release workflows
for t in ("goggle", "goggle2", "boxpro"):
    wf = open(os.path.join(ROOT, f".github/workflows/build_and_release_{t}.yml")).read()
    cond = re.search(r"if: (startsWith\(github\.ref, 'refs/tags/v'\) && contains\(github\.ref, '-juricabi\.'\))", wf)
    check(cond is not None and "ripples-v" not in wf, f"{t} workflow releases on a v*-juricabi.* tag (v{version})")

if "--images" in sys.argv:
    d = sys.argv[sys.argv.index("--images") + 1]
    for name, hal, files in (("HDZERO_GOGGLE", "goggle", ("HDZGOGGLE_RX", "HDZGOGGLE_VA")),
                             ("HDZERO_GOGGLE2", "goggle2", ("HDZGOGGLE_RX", "HDZGOGGLE_VA")),
                             ("HDZERO_BOXPRO", "boxpro", ("HDZBOXPRO_RX", "HDZBOXPRO_VA"))):
        hv = open(os.path.join(ROOT, f"mkapp/hal/{hal}/version")).read().strip()
        img = os.path.join(d, f"{name}-{hv}-{version}.bin")
        check(os.path.isfile(img), f"image {os.path.basename(img)} (no commit suffix)")
        if not os.path.isfile(img):
            continue
        sha = hashlib.sha256(open(img, "rb").read()).hexdigest()
        side = open(img + ".sha256").read().split()[0] if os.path.isfile(img + ".sha256") else ""
        check(side == sha, f"{os.path.basename(img)}.sha256 matches ({sha})")
        with tarfile.open(img) as tf:
            members = tf.getnames()
        check(f"hdzgoggle_app_ota-{version}.tar" in members and
              all(any(x.startswith(f) and x.endswith(".bin") for x in members) for f in files),
              f"{os.path.basename(img)} holds hdzgoggle_app_ota-{version}.tar and {', '.join(files)}: {members}")
    others = [f for f in os.listdir(d) if f.endswith(".bin") and version in f and not re.fullmatch(r"HDZERO_\w+-\d+-\d+-" + re.escape(version) + r"\.bin", f)]
    check(not others, f"no other image of {version} in {d} {others}")
print(f"{'all passed' if not fails else str(fails) + ' FAILED'}")
sys.exit(1 if fails else 0)
