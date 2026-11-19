#!/bin/bash
# End to end: record main loop (stubbed MPP) -> ring -> rtspLive -> RTSP clients, for H.264 and
# H.265, Stream Audio on and off, and frames that end in an SEI NAL unit (RTP marker placement).
#   usage: t_e2e.sh [h264|h265 ...]
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
L=$LOGS/e2e
rm -rf "$L"; mkdir -p "$L"
cleanup_ipc
CODECS=${*:-h264 h265}

for codec in $CODECS; do
    h265=$([ "$codec" = h265 ] && echo 1 || echo 0)
    for mode in on off sei; do
        audio=$([ $mode = off ] && echo 0 || echo 1)
        tag=${codec}_$mode
        echo "== $codec, Stream Audio $([ $audio = 1 ] && echo on || echo off)$([ $mode = sei ] && echo ', every frame ends with an SEI NAL unit')"
        write_conf "$L/$tag.conf" $audio $h265
        LAT_TRAILING_SEI=$([ $mode = sei ] && echo 1 || echo 0) start_record "$L/$tag.conf" "$L/record_$tag.log"
        start_live "$L/$tag.conf" "$L/rtspLive_$tag.log"

        s=$(probe_streams "$URL" | tr '\n' ' ')
        echo "  ffprobe: $s"
        vc=$([ $codec = h265 ] && echo hevc || echo h264)
        grep -q "codec_name=$vc" <<< "$s"; check $? "$tag: video $vc"
        if [ $audio = 1 ]; then
            grep -q "codec_name=aac|profile=LC|sample_rate=48000|channels=2" <<< "$s"; check $? "$tag: AAC LC 48 kHz stereo"
        else
            ! grep -q "aac" <<< "$s"; check $? "$tag: no audio track"
        fi

        python3 -I "$LAT_DIR/rtsp_rtp.py" "$URL" 6 "$L/rtp_$tag" > "$L/rtp_$tag.txt" 2>&1
        grep -E "^(video|audio)|NAL types|steps|a=fmtp|a=rtpmap" "$L/rtp_$tag.txt" | sed 's/^/  /'
        if [ $audio = 1 ]; then
            # stream-audio path: one access unit per encoder frame, marker on its last packet only
            read -r aus marked inside <<< "$(sed -n 's/^video: .* \([0-9]*\) complete access units, marker on the last packet of \([0-9]*\) of them, \([0-9]*\) markers.*/\1 \2 \3/p' "$L/rtp_$tag.txt")"
            [ -n "$aus" ] && [ "$aus" -gt 150 ] && [ "$aus" = "$marked" ] && [ "$inside" = 0 ]
            check $? "$tag: RTP marker on the last packet of every access unit ($marked/$aus), none inside ($inside)"
            grep -q "^  RTP timestamp steps (most common): 1024 x" "$L/rtp_$tag.txt"; check $? "$tag: audio RTP timestamps step by 1024"
        fi

        capture "$L/cap_$tag.nut" 10
        pc=$(packet_counts "$L/cap_$tag.nut")
        de=$(decode_errors "$L/cap_$tag.nut")
        echo "  10 s capture: $pc, decode errors: $de"
        read -r _ nv _ na <<< "$pc"
        [ "${nv:-0}" -ge 280 ]; check $? "$tag: video packets ($nv)"
        [ $audio = 0 ] || [ "${na:-0}" -ge 450 ]; check $? "$tag: audio packets (${na:-none})"
        [ "$de" = 0 ]; check $? "$tag: capture decodes without errors"
        stop_live
        stop_record
        cleanup_ipc
    done
done
summary
