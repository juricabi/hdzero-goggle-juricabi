# Live stream audio (RTSP AAC track)

The WiFi live stream (`rtsp://<goggle-ip>:8554/hdzero`) can carry the goggle's audio as an
AAC track next to the video. It is off by default: **WiFi Module > Page: System > Stream Audio**.
With it off the stream is the video-only stream it always was.

Applies to HDZero Goggle (WiFi on the expansion module), Goggle 2 (built-in WiFi) and BoxPro:
they share `record`, `rtspLive`, the record conf and the WiFi page.

**In juricabi's firmware** (ripples' firmware 1.2.2 with this project's additions, docs/status.md): ported
from `all` (082b2f5). `rtspLive` and the ring are `all`'s code (ripples had upstream's, but for
its flush in the writer's `avshare_reset()`, kept for the video side of the ring; since phase 4 the
audio side is left to the stream audio's writer, docs/status.md). ripples reworked the recorder
(`src/record/main.c`: the DVR control queue, rolling recordings), and the stream's hooks are
merged into it: `live_run()` (after ripples' pause check), the AI device's users
(`aidev_heldByOthers()`), `vvLive_onFrame()`'s clock, the stream's conf. The input is
whatever the Audio page records (ripples' Audio Source with the mixer's Record rows, docs/audio.md).

## 1. The live path (as found, before this change)

### Processes and configuration

| What | Where |
|---|---|
| `record` starts at boot and stays up (recording *and* live encoding) | `mkapp/app/script/rc.sh` |
| `rtspLive` (live555 RTSP server) starts with WiFi: the main app writes `/tmp/wlan_start_ap.sh` / `/tmp/wlan_start_sta.sh` ending in `/mnt/app/app/record/rtspLive > /tmp/rtspLive.log` | `src/ui/page_wifi.c` `page_wifi_update_services()` |
| WiFi off: `gogglecmd -live quit`, `killall rtspLive` | `mkapp/app/script/wlan_stop.sh` |
| Both read the record conf: `/data/confs/record.conf` if it exists, else `<exe dir>/confs/record.conf` = `/mnt/app/app/record/confs/record.conf`, which is `REC_CONF`, the file the main app writes (`src/core/dvr.c`) | `record_checkConf()` (`src/record/main.c`), `live_checkConf()` (`src/rtspLive/main.cpp`) |
| rtspLive settings: `[live] multicast` (shipped `0`: unicast RTSP), `port` 8554, `name` hdzero, `[venc_live] h265` (shipped `0`: H.264) | `src/rtspLive/stream/confparser.c` |

### Video, end to end

1. A client sends DESCRIBE. `H264LiveVideoServerMediaSubssion` (an `H264VideoFileServerMediaSubsession`,
   `reuseFirstSource`) builds its SDP from a dummy source: `H264FramedLiveSource` ->
   `avshare_start()` attaches the shared memory (resetting it) and sets the *video connected*
   byte; `getAuxSDPLine()` waits for SPS/PPS; the dummy is closed again (`avshare_stop()`).
   SETUP/PLAY create the real source the same way.
2. `record`'s main loop polls every 100 ms (`live_run()`): video connected -> `live_start()`: a
   second VI channel + VENC from `[venc_live]` (shipped: 1280x720, 30 fps, 2 Mbit/s, H.264 CBR);
   not connected -> `live_stop()`. The live encoder is independent of recording.
3. `vvLive_onFrame()`: the first frame must be an I frame (else an I frame is requested); before
   every I frame the SPS/PPS from `vi2venc_getSpsPpsInfo()` is queued as a frame of its own; each
   frame goes to the ring with `avshare_addvid()`.
4. rtspLive reads with `avshare_readFrame()` (all filled slots concatenated, as a byte stream) into
   `H264VideoStreamFramer`, which parses NAL units and **makes up its own presentation times**
   (wall clock at the first NAL + n / frame rate from the SPS, default 30). The timestamp in the
   ring (ms since the first live frame, passed through a `uint32_t`) was never used. The parser
   also holds the last NAL of each frame until the next frame's start code arrives (one frame of
   latency).

Multicast (`[live] multicast=1`, code default when the key is missing) uses a
`PassiveServerMediaSubsession` with the same source and framer.

### Audio in `record`

Only `record_start()` with `[record] audio=1` ("Record Audio") creates an AI channel bound to an
AENC channel (`src/record/ai2aenc.c`: `[ai]` 48 kHz / 16 bit / 2 ch from device 0, `[aenc]` AAC
64 kbit/s, `attachAACHeader = 1` -> ADTS), and `aa_onFrame()` hands the frames to the TS/MP4
muxer only. So **during live streaming there is no audio unless a recording with audio is
running, and never with Record Audio off**. Nothing feeds the audio side of the ring:
`avshare_addAudio()` (record) and `avshare_readAudio()` (rtspLive) are unused leftovers
(`addAudio` expects TS PES fragments; `readAudio` computes `tv_usec = ts*100/9` without a modulo).
Input routing is not touched by this work: the stream carries whatever the codec's ADC captures
on AI device 0, which is what the DVR records: the Audio page's recording rows (docs/audio.md: in
juricabi's firmware ripples' Audio Source, the preset of Record Mic / Line In / A/V In, Record Headphone
Mix and Line/AV Gain, the level). The headphone volume does not change it.

### The shared ring (`src/record/avshare.c` writer, `src/rtspLive/stream/avshare.c` reader)

SysV shm key `0x568066`, 632 578 bytes, identical constants on both sides:

| Offset | Size | Content |
|---|---|---|
| 0 | 2 | connect bytes: [0] video reader, [1] audio reader (set by rtspLive) |
| 2 | 56 x 16 | video slot headers |
| 898 | 56 x 16 | audio slot headers |
| 1 794 | 56 x 10 KiB | video slot data |
| 575 234 | 56 x 1 KiB | audio slot data |

Slot header, `#pragma pack(1)`, 16 bytes, same on both sides: `char wflag` (1 = free, 0 = filled),
`char frameType`, `char mediaType`, `char buff_type` (BEGIN / MIDDLE / END / BEG_END: a frame
spans one or more consecutive slots), `uint64_t timestamp`, `int length`.

Writer and reader keep their own slot index. The writer never overwrites a filled slot: when its
next slot is not free the frame is dropped (`avshare_add()` may leave the first slots of a
multi-slot frame behind) and video then drops everything until the next I frame. When the reader
finds its slot free it scans for any filled slot. Writing to a reader that is not connected
returns -3 and resets the writer to slot 0. The record process clears the connect bytes when it
starts, rtspLive when it first attaches; nothing cleared them when rtspLive died.

Defects found on the way (fixed where the new path relies on them):
- rtspLive `avshare_connect()`/`avshare_disconnect()` indexed by media *type* (1/2) instead of
  index (0/1): disconnecting video reset the **audio** headers, connecting audio wrote out of
  bounds. Fixed.
- No memory barriers between writing a slot and flipping `wflag`, nor between seeing a slot
  filled and reading it (ARMv7 is weakly ordered and does not order loads behind a control
  dependency). Added on both sides, for every slot, also on the audio-off path (`avshare_add()`,
  `avshare_readFrame()`): an acquire after each `wflag` test, a release before each flip.
- Video timestamps truncated to 32 bit by `avshare_add(..., uint32_t millis, ...)`. Now 64 bit.
- `audioStreamSize` / `largeStreamSize` macros are not parenthesized (`1 * 1024`). The new code
  parenthesizes its uses.
- `disk_checkFile()` (record `disk.c` and rtspLive `main.cpp`) tested `st_mode` after a failed
  `stat()`: a missing `/data/confs/record.conf` could be taken as present (stack garbage) and the
  process then ran on built-in defaults (rtspLive: multicast, H.265, no `[live] audio`). Fixed;
  likewise the `readlink("/proc/self/exe")` result used to find `<exe dir>/confs/record.conf`,
  which was not NUL-terminated (same effect, seen in the host tests).
- A stale connect byte (rtspLive killed by `killall` in `wlan_stop.sh`, SIGKILL, a crash) kept
  the live video encoder running until the next stream or a reboot. Fixed (see 2.).

### live555 in `lib/live`

Prebuilt static libraries, version 2022.12.01. Present and used: `OnDemandServerMediaSubsession`,
`FramedSource`, `MPEG4GenericRTPSink`, `H264VideoStreamDiscreteFramer`,
`H265VideoStreamDiscreteFramer` (virtual `nalUnitEndsAccessUnit()`), `RTCPInstance`.
Present but not suitable: `ADTSAudioFileServerMediaSubsession` / `ADTSAudioFileSource` (read a
file), `ADTSAudioStreamDiscreteFramer` (*adds* ADTS headers).

## 2. What was added

### Setting

- `[wifi] stream_audio` in `setting.ini`, default `false` (`src/core/settings.{c,h}`).
- WiFi Module, page System: **Stream Audio On/Off** between SSH and Apply Settings
  (`src/ui/page_wifi.c`, translated in `mkapp/app/language/*.ini`). Applying the WiFi settings
  (also done once after boot) writes `[live] audio=0|1` into the record conf **before** rtspLive
  is (re)started.
- `[live] audio` is read by rtspLive at start (`conf_loadLiveParams()`) and by `record` when the
  audio reader connects (`conf_loadLiveAudio()`). Shipped `record.conf` has `audio=0`.
- In juricabi's firmware the recorder normally runs on ripples' DVR config snapshots: the app copies
  `record.conf` once when it starts (`src/core/dvr.c`) and hands the recorder edited copies with
  its own `[vi]`, `[venc]` and `[record]` keys. A Stream Audio change after that, or a hand edit of
  `[live]`, would never be in a snapshot, so the stream's audio reads its conf (`[live]`, `[ai]`,
  `[aenc]`, which the snapshots never change) from the file the recorder chose at its start
  (`liveConfFile` in `src/record/main.c`: the app's `record.conf`, the file the WiFi page writes
  and rtspLive reads). `t_snapshot.sh` checks it in both directions.
- `[live] audio_offset_ms` (shipped `0`, -500..500): added to the stream's audio timestamps,
  positive delays the audio. Only needed if the video pts clock turns out not to be
  CLOCK_MONOTONIC on a goggle (see "Timestamps" and 4.). Read at each start of the audio chain.

### `record` (`src/record/live_audio.{c,h}`, hooks in `main.c`)

- `liveaudio_run()` is called from `live_run()`. When rtspLive connects the audio side of the ring
  (and `[live] audio=1`), a **dedicated AI -> AENC chain** (`ai2aenc` instance of its own, AAC,
  ADTS, `[ai]`/`[aenc]` conf) starts, independent of recording and of Record Audio. It stops 3 s
  after the reader goes away (DESCRIBE and SETUP each connect, the delay avoids a restart in
  between); a failed start is retried after 5 s; a running chain that produces no frame for 2 s
  is stopped and started again. In juricabi's firmware the poll comes after ripples' pause check:
  while the recorder is paused around a source switch the chain is neither started nor stopped
  (it runs on; ripples' `avshare_reset()` then flushes the video side of the ring for the new
  video bitstream, and leaves the audio side to the stream audio's writer: no audio frame is
  lost, `t_pause.sh`).
- The chain captures from the same `[ai] device` as recording: the stream carries what the DVR
  records (the Audio page's recording rows). HDMI-in audio is in it only with HDMI In Audio and
  Record Headphone Mix on (docs/hdmi-audio.md).
- Users of the AI device: recording with audio, the AI -> AO loop, the stream audio and the HDMI
  out audio bridge (`pcmbridge.c`, route `src=ai`) can run at the same time (channels on one
  device). `aidev_heldByOthers(recCtx, self)` in `main.c` answers whether a user other than
  `self` holds the device; every place that stops a user asks it before disabling the device
  (`record_stop()`, `AIO_cmdSTOP`, `HDMIA_cmdSTART` / `HDMIA_cmdSTOP`, the live audio poll, exit,
  in every order). Further users register an `AiUser_e` entry and one line in its table.
  `t_aidev.sh` checks with the stub AI device that it is enabled exactly while a user holds a
  channel: every start/stop order of recording, stream and bridge, every stop order with all
  four running, the bridge route replaced by the HDMI-in route, a bridge start failing after
  taking its channel, and quitting with each set of users running.
- `liveaudio_onFrame()` queues each ADTS frame **with its header** via the new
  `avshare_addAudioFrame()`: all or nothing (a frame never ends up half in the ring), slots
  published in order behind a barrier. Ring full -> the frame is dropped and counted.
- Reader liveness: rtspLive holds a write lock on `/tmp/live.pid` while it runs
  (`already_running()`, also in earlier versions); the kernel drops it however the process ends.
  `avshare_connected()` ignores a connect byte while nobody holds that lock (checked with
  `F_GETLK` at most every 500 ms, only while a byte is set; without the pid file the byte is
  trusted as before). So a killed or crashed rtspLive no longer leaves the live video encoder or
  the audio chain running. The shared layout is unchanged; an old rtspLive or an old record on
  the other side works as before.

#### Timestamps

All ring timestamps are **CLOCK_MONOTONIC us**, the clock rtspLive turns into wall-clock
presentation times.

- `liveclock_stamp()` maps an encoder pts: MPP stamps captured audio with
  `CDX_GetSysTimeUsMonotonic()` minus the chunk duration (seen in `audioHw_AI_CapThread`); a pts
  up to 1 s in the past is taken as the capture time as it is, any other clock is anchored to the
  arrival of the first frame and then follows the encoder pts (re-anchored if the stamps leave
  [now - 2 s, now + 0.2 s]). Live video frames (`vvLive_onFrame()`) get the same treatment.
  At each stream start the record log says per medium which case it is:
  `live video: pts clock looks like CLOCK_MONOTONIC, frame captured 15.2 ms before it reached the ring`
  or `live video: pts clock is not CLOCK_MONOTONIC (now - pts = ... ms): stamped on arrival, ...`.
  The video pts is the V4L2 buffer timestamp (`libmpp_vi.so` calls no clock); if it is not
  CLOCK_MONOTONIC on a goggle, video is stamped late by its capture-to-encoder latency and lags
  the audio by that much: `[live] audio_offset_ms` makes up for it.
- Audio: the presentation time of each AAC frame comes from a **sample counter** (1024 samples
  per raw block, rate from the ADTS header) anchored to the first capture time, not from the
  per-chunk capture stamps (which jitter with the capture thread): the RTP clock advances by
  exactly one frame per frame. It follows the mean capture time slowly (at most 20 us per frame,
  below one RTP tick at 48 kHz, for drift between the codec's sample clock and CLOCK_MONOTONIC)
  and re-anchors when the capture times are more than 15 ms off for 8 frames in a row (lost
  samples, clock step), not on one late chunk. Then `[live] audio_offset_ms` is added.

### `rtspLive`

- `[live] audio=1`: the session gets an `AACLiveAudioServerMediaSubsession` (`track2`) and the video
  subsession switches to `LiveNalVideoSource` + `Live{H264,H265}VideoStreamDiscreteFramer`. With
  `audio=0` the server builds exactly the session it built before (same classes, same source;
  SDP and packets checked identical to 8364a43, see 3.; 8364a43 has the same rtspLive code as
  the base a85841d, and ripples 4fbb865 has it unchanged too).
- Audio: `LiveAudioSource` reads one frame at a time (`avshare_readMediaFrame()`), checks and strips
  the ADTS header, delivers the raw AAC access unit with presentation time = ring timestamp
  converted to wall clock (`avshare_timestampToTimeval()`: one process-wide CLOCK_MONOTONIC ->
  gettimeofday offset, so audio and video stay consistent and RTCP sender reports, which pair
  wall clock with RTP time, are right). Frames older than 1 s are skipped: a paused or stalled
  reader can be up to 1 s behind (the ring holds about 1.2 s of audio), then it skips the backlog.
  The timestamps stay right in either case; a player that syncs on them plays in sync, one that
  plays audio as it arrives can lag by that much.
- It polls the ring paced by the frame duration: after a frame it sleeps until 4 ms before the
  next one is due, then polls every 2 ms (about 90 wake-ups/s instead of 500). The record side
  already blocks on the encoder (`AW_MPI_AENC_GetStream()`, 100 ms timeout); the ring has no wait
  primitive. Video keeps its 1 ms polling (both paths, as before).
- Payload: RFC 3640 `MPEG4-GENERIC`, mode `AAC-hbr`, one AU per RTP packet, RTP clock = sample
  rate. SDP for the shipped conf:
  ```
  m=audio 0 RTP/AVP 97
  a=rtpmap:97 MPEG4-GENERIC/48000/2
  a=fmtp:97 streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1190
  ```
  `config` is the AudioSpecificConfig built from the **first ADTS header the encoder actually
  produces** (object type = profile + 1, frequency index, channel configuration; AAC-LC 48 kHz
  stereo = `1190`); frames in another format later are dropped by the sources that announced it.
- **Waiting for the first audio frame at DESCRIBE.** live555 asks a subsession for its SDP lines
  twice per DESCRIBE and does not keep a NULL answer, and DESCRIBEs of different clients nest
  (the wait runs a nested event loop). So there is one wait per process: one 2.5 s deadline
  shared by all waiting DESCRIBEs (each with its own done flag, all return together), and a
  timeout is remembered for 10 s, during which DESCRIBEs leave the audio track out at once
  (`sdpLines()` returns no lines without opening a source). A DESCRIBE is delayed at most 2.5 s,
  and at most once per 10 s; with audio flowing it is not delayed (only the very first DESCRIBE
  waits for the chain to start, ~0.3 s). The track format is process-wide (`LiveAudioTrack`),
  noted from every frame any source or the check sees.
- **When the audio goes away.** An audio source that has waited 6 s for a frame (longer than
  one record retry period) gives the track up process-wide: the next DESCRIBE checks again (one
  bounded wait) and leaves the track out if no audio comes. Frames seen again announce it again.
  A client that already set the track up keeps it and gets audio again when it comes back.
- Video with audio on: each ring frame (`avshare_readMediaFrame()`) is split into NAL units, start
  codes removed, every NAL carries the frame's own timestamp. The RTP marker is set on the **last
  NAL unit of a ring frame that holds a picture** (a VCL NAL unit): the access unit is what shares
  the timestamp, so a trailing SEI (H.265 suffix SEI, end of sequence) gets it; a ring frame
  without a picture (the parameter sets queued before an I frame) ends nothing. No NAL waits for
  the next frame, so this path is one frame *faster* than the parser path.
- Ring reader (`src/rtspLive/stream/avshare.c`): media attached/connected per media with a user
  count (`avshare_startMedia()` / `avshare_stopMedia()`; `avshare_start()`/`avshare_stop()` are the
  video case, unchanged in effect), connect resets that media's slots, frame-by-frame reader with
  resync, release of orphan slots, `avshare_peekAudio()`. A frame the writer gave up half-way is
  dropped when the next frame start appears, or when it has not grown for 100 ms: otherwise its
  slots stay taken and an I frame larger than the rest of the ring (above ~280 KiB) stays partial
  behind it, frame after frame.
- Exit: rtspLive clears its connect bytes at exit, on SIGTERM/SIGINT/SIGHUP (then dies of the
  signal as before) and in the SIGSEGV handler.
- Multicast mode: audio is not supported (logged), video only as before.

### Behaviour summary

| Situation | Stream |
|---|---|
| Stream Audio off | video only, unchanged path (SDP and packets as before) |
| On, audio chain runs | video (NAL path) + AAC, shared clock |
| On, no audio frame (chain failed / not producing) | video only (track left out of SDP); a DESCRIBE waits at most 2.5 s, at most once per 10 s |
| On, audio stops for good while streaming | record restarts / retries the chain; after 6 s rtspLive gives the track up: new DESCRIBEs get video only until audio is back |
| On, Record Audio off | audio present (live chain does not depend on it) |
| On, recording with audio / AI->AO loop at the same time | all run; start/stop of one does not stop the others or disable the device under them |
| Client paused / slow | ring holds ~1.2 s of audio, then drops new frames; reader skips frames older than 1 s |
| rtspLive killed / crashed | record stops the live video encoder within ~0.5 s and the audio chain 3 s later |
| juricabi: a source switch while streaming (the recorder paused, then resumed) | live video stops and comes back; the audio runs on (no frame lost: no pts gap over 1.5 AAC frames, 32 ms) |
| Several clients | one shared source per track (`reuseFirstSource`), the ring has one reader per media |

## 3. Tests on the build host

`utilities/live_audio_test/` (not part of the firmware build): `run_all.sh` builds and runs
everything; `common.sh` says what it needs (`LIVE555`: a built host live555 for the native
rtspLive and openRTSP; `TOOLCHAIN`: the buildroot SDK for the ARM runs; ffmpeg with libx264,
libx265, python3, qemu-arm). The programs use the goggle's fixed paths and IPC keys
(`/tmp/live.pid`, `/tmp/record.pid`, SysV shm 0x568066, message queues): one test at a time.

- `record_host`: the **real** `src/record/main.c` main loop, `live_audio.c`, `avshare.c`,
  `confparser.c`, `gogglemsg.c` over `record_stubs.c` (MPP VI/VENC/AI/AENC/AO, muxer, SD card),
  driven by goggle messages (`gmsg`: record start/stop, AI->AO loop on/off, quit) and fed in real
  time from `gen_media.sh` media (moving pattern, 2 Mbit/s, 4 slices per picture, I frames over
  one ring slot; flash + beep markers at aperiodic instants). Knobs: audio pts 0..J us late,
  video pts on a foreign clock, an SEI NAL at the end of every frame, a failing AI chain. The stub
  AI device reports any disable while another channel uses it and a device left enabled.
- `rtspLive`: the real `src/rtspLive` native against host live555 2023.06.14 (same classes as
  the prebuilt 2022.12.01), or the cross-built binary with the prebuilt library under qemu-arm.
- Clients: ffprobe, ffmpeg captures, `rtsp_describe.py` (timed DESCRIBEs, overlapping),
  `rtsp_rtp.py` (RTP over RTSP/TCP: markers per access unit, NAL types, timestamp steps).

| Test | Result (as measured on `all`; in juricabi's firmware: the last two rows and docs/status.md, phase 2) |
|---|---|
| `ring_test` (x86, and ARM under qemu-arm) | 7254 checks, 0 failures: frames of 1..3 slots across wrap-arounds, overrun (whole frames dropped), peek, reconnect, partial-frame recovery, I frames of 40 slots (recover at the 2nd I frame; never without the 100 ms rule), legacy path, 200 000 frames with concurrent threads, a dead reader's stale flag ignored |
| `t_describe.sh` (no audio produced) | first DESCRIBE 2.91 s (video start + 2.5 s), within the retry period 0.002 s, after it 2.51 s; three DESCRIBEs at +0 / +1.0 / +1.5 s all done at +2.51 s; was 5.1..5.4 s each, and LIFO |
| `t_describe.sh` (audio produced) | first DESCRIBE 0.56 s, then 0.002 s |
| `t_kill.sh` (quit / TERM / KILL while streaming) | live video encoder stopped after 0.12 / 0.12 / 0.07 s, audio chain after 3.00 / 3.04 / 2.98 s, nothing written afterwards, a new rtspLive streams both tracks; was: never stopped |
| `t_share.sh` | recording with audio and AI->AO loop started/stopped while streaming, a stream during a recording: no device misuse, largest gap in the stream audio 21 ms |
| `t_e2e.sh` H.264 / H.265, on / off / trailing SEI | ffprobe tracks, 10 s captures without decode errors, RTP marker on the last packet of every access unit (on the SEI when there is one), none inside, audio RTP steps 1024 |
| `t_avsync.sh` | lip sync error against the source files: 0.0 ms; audio pts 0..8 ms late: 4.0 ms = the mean lateness (24.9..25.5 ms, was 4.9 ms mean, 21.5..28.9), RTP steps all 1024 +-1 (were 842..1083); video pts on a foreign clock: -15.1 ms (the stub latency), logged; with `audio_offset_ms=15`: -0.1 ms |
| `t_audio_fail.sh` | chain dies at +6 s: track given up, DESCRIBE at +14 s video-only (2.50 s), record restarts and retries, back at +18 s: DESCRIBE at +28 s with audio (0.002 s), the streaming client has audio again |
| `t_cpu.sh` (host) | rtspLive with audio 1375 -> 1066 wake-ups/s, 3.8 -> 3.0 % CPU; audio off 970..994 |
| `t_off_identical.sh` | Stream Audio off: SDP (but `o=`) and RTP packets (marker, size, payload header, timestamp steps) identical to the 8364a43 rtspLive, H.264 and H.265 |
| `t_arm.sh` | the shipped ARM rtspLive (live555 2022.12.01) under qemu-arm: `t_e2e.sh` 38/38, `t_describe.sh` 12/12 (2.82 s / 2.51 s / overlap +2.51 s / 0.57 s with audio), `t_kill.sh` 15/15 |
| `t_snapshot.sh` (juricabi) | `ctl_conf` publishes a config snapshot and sends `RECORD_CONFIG` like the app's DVR control; then Stream Audio changes in the recorder's start file: snapshot `audio=0` + Stream Audio On -> SDP with audio, the chain starts, frames reach the ring; snapshot `audio=1` + Stream Audio Off -> video-only SDP, "audio is off", no chain. 9/9; without the fix 2/9 (the recorder followed the snapshot) |
| `t_pause.sh` (juricabi) | while a client streams with audio, `ctl_conf` pauses the recorder (`RECORD_PAUSE`, as the app does before a source switch: live video stops, `avshare_reset()` flushes the video side of the ring) and resumes it 2 s later (`RECORD_LIVE_STOP` with a snapshot): the audio chain keeps running, no audio frame is lost (no pts gap over 32 ms, 1.5 AAC frames of 21.3 ms) and the audio stays in order, video is back after the resume. 7/7 |

Not testable without a goggle: the MPP AI/AENC chain itself (two or three AI channels at once,
its real ADTS output and timestamps, chunk sizes, capture jitter), the VI pts clock, the
Allwinner encoder's NAL layout, the camera-to-goggle latency, memory ordering on the Cortex-A7
(x86 and qemu-arm on x86 are both TSO), WiFi throughput, CPU load on the goggle, the UI page.

### Known limitations

- Multicast mode (`[live] multicast=1`): video only.
- A goggle with a legacy `/data/confs/record.conf`: rtspLive reads that file (as upstream and
  ripples do), the WiFi page writes the app's `record.conf`, so Stream Audio has no effect there
  (the same on `all`; in juricabi's firmware the recorder prefers the app's file, ripples' rule).
- DESCRIBE waits for the first AAC frame (normally a few hundred ms; 2.5 s, once per 10 s, if
  none comes).
- Recording with audio while streaming encodes the same PCM twice. One encoder for both was not
  done: the recording chain runs only while recording, with its own codec and pts bases, and the
  stream would have to switch encoders at every recording start/stop (gap or overlap, timeline
  re-anchor, possible format change) to save one AAC-LC encoder while both run.

## 4. Testing on a goggle

1. WiFi Module > System > Stream Audio: On, Apply Settings. `grep audio /mnt/app/app/record/confs/record.conf`
   shows `audio=1` under `[live]`; `/tmp/rtspLive.log` shows `live audio: on`.
2. `ffprobe -rtsp_transport tcp rtsp://192.168.2.122:8554/hdzero` (AP mode default address) lists
   `Video: h264` and `Audio: aac (LC), 48000 Hz, stereo`; `/tmp/rtspLive.log` shows
   `live audio: AAC object type 2, 48000 Hz, 2 channels, config=1190`.
3. Play: `ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay rtsp://192.168.2.122:8554/hdzero`
   or VLC "Open Network Stream" with the same URL (`vlc --network-caching=300 rtsp://192.168.2.122:8554/hdzero`).
   Check sound from the selected input, latency unchanged, CPU (`top`).
4. **Pts clock and lip sync** (decides whether `[live] audio_offset_ms` is needed):
   1. Put an empty file `RECORD.log` in the root of the SD card and reboot: the record process
      then logs to it (`mkapp/app/script/rc.sh`).
   2. On a PC: `WORK=/tmp/lat utilities/live_audio_test/gen_media.sh`, then play
      `/tmp/lat/media/source_ref.mkv` full screen in front of the HDZero camera (`mpv --fs`), the
      PC's audio output into the goggle's line in (no acoustic delay), Audio Source Line In.
      The file itself has its beeps 21.3 ms after the flashes (AAC encoder delay):
      `python3 utilities/live_audio_test/avsync.py /tmp/lat/media/source_ref.mkv` prints it.
   3. With the PC on the goggle's WiFi: `ffmpeg -rtsp_transport tcp -i rtsp://192.168.2.122:8554/hdzero -t 40 -c copy -f nut cap.nut`
      and `python3 utilities/live_audio_test/avsync.py cap.nut 5`: mean beep - flash in ms.
   4. On the goggle: `grep "pts clock" /mnt/extsd/RECORD.log`: one `live video:` and one
      `live audio:` line per stream start.
   5. Expected: both `looks like CLOCK_MONOTONIC`, and (mean - 21.3 ms) close to minus the
      camera-to-goggle video latency (screen + camera + link: a few ms up to about one frame),
      the same as a DVR recording of the same scene (`avsync.py` on the .ts file). If the video
      line says `is not CLOCK_MONOTONIC`, or the stream's beep comes clearly earlier than the
      recording's (by more than a frame), set `audio_offset_ms=<that many ms>` under `[live]` in
      `/mnt/app/app/record/confs/record.conf`, stop streaming for 5 s (the chain restarts and
      reads it), and repeat 3. (A firmware update restores the shipped conf.)
5. Repeat 3 with Record Audio off; while recording with audio (both the recording and the stream
   have sound; stop/start recording during streaming: the stream audio keeps going); with the
   AI->AO monitor on.
6. Without audio: with Stream Audio On and rtspLive running (it reads `[live] audio` at start),
   set `audio=0` under `[live]` in record.conf and restart `record` (it reads the key when the
   reader connects): `time ffprobe ...` takes at most ~2.5 s more than with audio, a second one
   right after it none; the SDP is video-only. Set it back and restart `record`.
7. WiFi off while a client streams, and `killall -9 rtspLive` while a client streams: `top`
   shows no live encoding left in `record` after ~3 s (RECORD.log: `stream reader gone ...`,
   `stopped (... frames ...)`); the next stream works.
8. Stream Audio Off: SDP video-only as on the previous firmware.
9. Long run (30 min) for A/V drift: `ffmpeg -rtsp_transport udp -i rtsp://192.168.2.122:8554/hdzero -t 1800 -c copy long.mkv`,
   `avsync.py` on its first and last minutes (cut with `ffmpeg -ss`).
10. Same on Goggle V1 (WiFi module) and BoxPro.
