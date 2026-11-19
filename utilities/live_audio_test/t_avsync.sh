#!/bin/bash
# Audio timestamps and lip sync with imperfect encoder timestamps (record_stubs.c knobs):
#   ideal    capture times as pts on CLOCK_MONOTONIC
#   jitter   audio pts 0..8 ms late (MPP stamps PCM chunks when its capture thread reads them):
#            the jitter must go, the mean lateness (4 ms) stays
#   foreign  video pts on another clock: video is stamped on arrival, 15 ms late (stub latency)
#   offset   foreign, with [live] audio_offset_ms=15 to make up for it
# Checked: audio RTP timestamps advance by one AAC frame (1024 at 48 kHz), the A/V offset of the
# flash/beep markers in a capture against the same offset in the source files, and what the
# record log says about the pts clocks.
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
L=$LOGS/avsync
rm -rf "$L"; mkdir -p "$L"
cleanup_ipc

REF=$(python3 -I "$LAT_DIR/avsync.py" "$MEDIA/source_ref.mkv" | sed -n 's/.*mean=\([-0-9.]*\).*/\1/p')
echo "source files: audio - video offset $REF ms (AAC encoder delay)"
within() { awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= t) }'; }

for case in ideal jitter foreign offset; do
    extra=()
    envs=()
    expect=0
    case $case in
        jitter) envs=(LAT_AUDIO_JITTER_US=8000); expect=4 ;; # the stamps are 4 ms late on average
        foreign) envs=(LAT_VIDEO_CLOCK=foreign); expect=-15 ;;
        offset) envs=(LAT_VIDEO_CLOCK=foreign); extra=(audio_offset_ms=15) ;;
    esac
    echo "== $case ${envs[*]} ${extra[*]}"
    write_conf "$L/$case.conf" 1 0 "${extra[@]}"
    for e in "${envs[@]}"; do export "${e?}"; done
    start_record "$L/$case.conf" "$L/record_$case.log"
    for e in "${envs[@]}"; do unset "${e%%=*}"; done
    start_live "$L/$case.conf" "$L/rtspLive_$case.log"

    python3 -I "$LAT_DIR/rtsp_rtp.py" "$URL" 6 "$L/rtp_$case" > "$L/rtp_$case.txt" 2>&1
    read -r total exact <<< "$(awk '$1 == "audio" && $5 > 0 { n++; if ($5 >= 1023 && $5 <= 1025) e++ } END { print n + 0, e + 0 }' "$L/rtp_$case.pkts")"
    echo "  audio RTP timestamp steps: $exact of $total within 1024 +- 1 ($(grep -A2 '^audio' "$L/rtp_$case.txt" | grep steps | sed 's/^ *//'))"
    [ "$total" -gt 200 ] && [ "$exact" -ge $(( total * 98 / 100 )) ]; check $? "$case: audio RTP clock smooth ($exact/$total steps of one frame)"

    capture "$L/cap_$case.nut" 16
    r=$(python3 -I "$LAT_DIR/avsync.py" "$L/cap_$case.nut" 4)
    m=$(sed -n 's/.*mean=\([-0-9.]*\).*/\1/p' <<< "$r")
    err=$(awk -v m="$m" -v r="$REF" 'BEGIN { printf "%.1f", m - r }')
    echo "  capture: $r -> lip sync error $err ms (expected $expect)"
    within "$err" "$expect" 4; check $? "$case: lip sync error $err ms, expected $expect +- 4 ms"
    grep -E "pts clock|re-anchored|audio offset" "$L/record_$case.log" | sed 's/^[ 0-9.]*\[rec [a-z_]*\] /    record: /'
    if [ $case = foreign ] || [ $case = offset ]; then
        grep -q "live video: pts clock is not CLOCK_MONOTONIC" "$L/record_$case.log"; check $? "$case: the record log tells the video pts clock is not CLOCK_MONOTONIC"
    else
        grep -q "live video: pts clock looks like CLOCK_MONOTONIC" "$L/record_$case.log"; check $? "$case: the record log tells the video pts clock is CLOCK_MONOTONIC"
    fi
    stop_live
    stop_record
    cleanup_ipc
done
summary
