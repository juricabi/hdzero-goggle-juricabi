#!/bin/bash
# Builds the flashable firmware of HDZGOGGLE, HDZGOGGLE2 and HDZBOXPRO from a worktree and puts
# the .bin files (with .sha256) into /root/hdz/out/.
#
#   /root/hdz/build_all.sh <worktree> [goggle|goggle2|boxpro ...]
#
# The .bin is what the repository's own release build makes: CMake target HDZGOGGLE-OTA ->
# mkapp/mkapp_ota.sh, which packs mkapp/app (with the app, record and rtspLive) into the jffs2
# app.fex, tars it with the HAL files of the target and names it
# <HDZERO_GOGGLE|HDZERO_GOGGLE2|HDZERO_BOXPRO>-<HAL version>-<VERSION>-<commit>.bin, or, when HEAD is at a
# git tag, ...-<VERSION>.bin (mkapp_ota.sh get_app_version).
#
# Everything runs on the WSL file system (the image keeps the file modes it finds; a Windows
# drive would hand mkfs.jffs2 its own): the build directories are
# /root/hdz/out/build/<worktree name>/<target>, outside the worktree, so its git status stays
# clean; the version and commit come from the worktree's git (GIT_DIR). A worktree on a
# Windows drive (/mnt/...) is refused.
#
# Environment: TOOLCHAIN (default /root/hdz/toolchain), OUT (default /root/hdz/out), JOBS.
set -u -o pipefail

W=${1:?usage: $0 <worktree> [goggle|goggle2|boxpro ...]}
shift
TARGETS=${*:-goggle goggle2 boxpro}
TOOLCHAIN=${TOOLCHAIN:-/root/hdz/toolchain}
OUT=${OUT:-/root/hdz/out}
JOBS=${JOBS:-$(nproc)}

W=$(cd "$W" 2>/dev/null && pwd -P) || { echo "no such directory: $1" >&2; exit 2; }
[ -f "$W/CMakeLists.txt" ] && [ -f "$W/mkapp/mkapp_ota.sh" ] || { echo "$W is not a goggle firmware tree" >&2; exit 2; }
for d in "$W" "$OUT"; do
    mkdir -p "$d"
    fs=$(stat -f -c %T "$d")
    case "$fs" in
        ext2/ext3|ext4|btrfs|xfs|tmpfs|overlayfs) ;;
        *) echo "$d is on '$fs', not the WSL file system: clone the worktree under /root first" >&2; exit 2 ;;
    esac
done
TCFILE=$TOOLCHAIN/share/buildroot/toolchainfile.cmake
[ -f "$TCFILE" ] || { echo "no toolchain at $TOOLCHAIN (set TOOLCHAIN)" >&2; exit 2; }
command -v mkfs.jffs2 > /dev/null || [ -x "$W/mkapp/bin/mkfs.jffs2" ] || { echo "mkfs.jffs2 missing (apt install mtd-utils)" >&2; exit 2; }

GIT_DIR=$(git -C "$W" rev-parse --absolute-git-dir) || { echo "$W is not a git worktree" >&2; exit 2; }
export GIT_DIR GIT_WORK_TREE="$W"
COMMIT=$(git rev-parse --short HEAD)
BRANCH=$(git rev-parse --abbrev-ref HEAD)
# the name mkapp_ota.sh gives the image: the version alone at a tag, else the version and the commit
if TAG=$(git describe --exact-match --tags HEAD 2> /dev/null); then
    SUFFIX="$(cat "$W/VERSION")"
else
    TAG=
    SUFFIX="$(cat "$W/VERSION")-$COMMIT"
fi
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "WARNING: $W has uncommitted changes; they are built, but the file name says $COMMIT"
fi

NAME=$(basename "$W")
mkdir -p "$OUT/logs" "$OUT/build/$NAME"
# mkapp_ota.sh takes the version from ../VERSION of the build directory
cp -f "$W/VERSION" "$OUT/build/$NAME/VERSION"
echo "worktree $W ($BRANCH @ $COMMIT${TAG:+, tag $TAG}), version $(cat "$W/VERSION"), images *-$SUFFIX.bin, targets: $TARGETS"

rc=0
for t in $TARGETS; do
    case $t in
        goggle) opt="-DHDZ_GOGGLE=ON -DHDZ_GOGGLE2=OFF -DHDZ_BOXPRO=OFF" ;;
        goggle2) opt="-DHDZ_GOGGLE=OFF -DHDZ_GOGGLE2=ON -DHDZ_BOXPRO=OFF" ;;
        boxpro) opt="-DHDZ_GOGGLE=OFF -DHDZ_GOGGLE2=OFF -DHDZ_BOXPRO=ON" ;;
        *) echo "unknown target $t" >&2; rc=1; continue ;;
    esac
    B=$OUT/build/$NAME/$t
    LOG=$OUT/logs/$NAME-$t.log
    mkdir -p "$B"
    # the packaging step's output is out/HDZGOGGLE: without it the image is always made again
    # (the version, the commit and mkapp/ may have changed without any source file)
    rm -f "$B"/out/*.bin "$B/out/HDZGOGGLE"
    echo "== $t: building in $B (log $LOG)"
    {
        cmake -S "$W" -B "$B" $opt -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$TCFILE" &&
        make -C "$B" -j"$JOBS"
    } > "$LOG" 2>&1
    r=$?
    # compiler warnings (the linker's 'liblog.so ... not found' of the prebuilt libadecoder is old)
    warn=$(grep 'warning:' "$LOG" | grep -vc 'liblog.so, needed by')
    if [ $r -ne 0 ]; then
        echo "   FAILED (exit $r):"; grep -m 20 -E 'error|Error' "$LOG" | sed 's/^/   /'
        rc=1
        continue
    fi
    bins=("$B"/out/HDZERO_*.bin)
    if [ ${#bins[@]} -ne 1 ] || [ ! -f "${bins[0]}" ]; then
        echo "   no single .bin in $B/out"; rc=1; continue
    fi
    bin=$(basename "${bins[0]}")
    case "$bin" in
        HDZERO_*-[0-9]*-[0-9]*-"$SUFFIX".bin) ;;
        *) echo "   $bin is not named *-$SUFFIX.bin"; rc=1; continue ;;
    esac
    cp -f "${bins[0]}" "$OUT/$bin"
    (cd "$OUT" && sha256sum "$bin" > "$bin.sha256")
    echo "   $OUT/$bin  $(stat -c %s "$OUT/$bin") bytes, $warn compiler warnings"
    echo "   sha256 $(cut -d' ' -f1 "$OUT/$bin.sha256")"
    tar tf "$OUT/$bin" | sed 's/^/     /'
    [ "$warn" -eq 0 ] || grep 'warning:' "$LOG" | grep -v 'liblog.so, needed by' | sed 's/^/   /' | head -20
done
exit $rc
