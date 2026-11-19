// Native unit test of the avshare ring: the record (writer) side and the rtspLive (reader) side
// are both linked in, their globals renamed w_* / r_* with objcopy, and they share the real
// SysV shared memory segment exactly as the two processes do on the goggle.
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { MEDIA_VIDEO = 1, MEDIA_AUDIO = 2 };
enum { FRAME_I = 1, FRAME_P = 2, FRAME_SPS = 4 };

// writer (src/record/avshare.c)
int  w_avshare_init(void);
int  w_avshare_uninit(void);
int  w_avshare_addAudioFrame(const uint8_t *data, int len, uint64_t timestampUs);
void w_avshare_addvid(uint8_t *pstStream, int len, int frameType, uint64_t timestamp);
bool w_avshare_connected(int streamType);

// reader (src/rtspLive/stream/avshare.c)
int  r_avshare_startMedia(int media_type);
void r_avshare_stopMedia(int media_type);
int  r_avshare_readMediaFrame(int media_type, uint8_t *buf, int bufLen, uint64_t *timestampUs);
int  r_avshare_peekAudio(uint8_t *buf, int bufLen);
void r_avshare_timestampToTimeval(uint64_t timestampUs, struct timeval *tv);
bool r_avshare_start(void);
void r_avshare_stop(void);
int  r_avshare_readFrame(uint8_t *sFrameBuf, int nBufLen, struct timeval *timestamp, unsigned *dur);

static int failures = 0;
static int checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// synthetic ADTS frame: valid 7-byte header (AAC-LC, 48 kHz, stereo) + payload pattern from seq
static int make_adts(uint8_t *buf, int payloadLen, uint32_t seq) {
    int frameLen = payloadLen + 7;
    buf[0] = 0xFF;
    buf[1] = 0xF1;
    buf[2] = (1 << 6) | (3 << 2) | (2 >> 2);
    buf[3] = ((2 & 3) << 6) | ((frameLen >> 11) & 0x03);
    buf[4] = (frameLen >> 3) & 0xFF;
    buf[5] = ((frameLen & 7) << 5) | 0x1F;
    buf[6] = 0xFC;
    for (int i = 0; i < payloadLen; i++) buf[7 + i] = (uint8_t)(seq * 31 + i * 7);
    if (payloadLen >= 4) memcpy(buf + 7, &seq, 4);
    return frameLen;
}

static bool check_adts(const uint8_t *buf, int len, uint32_t seq, int payloadLen) {
    uint8_t ref[8192];
    int n = make_adts(ref, payloadLen, seq);
    return (n == len) && (memcmp(ref, buf, len) == 0);
}

static int payload_for(uint32_t seq) {
    // mostly small frames, some spanning 2-3 slots of 1 KB, a few exactly at boundaries
    static const int sizes[] = {150, 171, 1017, 1018, 1500, 2041, 2042, 3000, 90, 400};
    return sizes[seq % 10] + (int)(seq % 13);
}

static void test_audio_basic(void) {
    uint8_t buf[8192] = {0}, out[8192];
    uint64_t ts;

    CHECK(w_avshare_addAudioFrame(buf, 100, 1) == -3, "write without reader must report not connected");

    CHECK(r_avshare_startMedia(MEDIA_AUDIO) == 0, "reader start");
    CHECK(w_avshare_connected(MEDIA_AUDIO), "writer sees audio reader");
    CHECK(!w_avshare_connected(MEDIA_VIDEO), "writer sees no video reader");

    CHECK(r_avshare_readMediaFrame(MEDIA_AUDIO, out, sizeof(out), &ts) == 0, "empty ring reads nothing");

    // push/read across many wrap-arounds of the 56-slot ring
    uint32_t seq;
    for (seq = 0; seq < 2000; seq++) {
        int len = make_adts(buf, payload_for(seq), seq);
        int ret = w_avshare_addAudioFrame(buf, len, 1000000ull + seq * 21333ull);
        CHECK(ret == 0, "write %u failed %d", seq, ret);
        if (seq % 3 == 2) { // read in bursts of 3
            for (uint32_t k = seq - 2; k <= seq; k++) {
                int n = r_avshare_readMediaFrame(MEDIA_AUDIO, out, sizeof(out), &ts);
                CHECK(n > 0 && check_adts(out, n, k, payload_for(k)), "frame %u content (len %d)", k, n);
                CHECK(ts == 1000000ull + k * 21333ull, "frame %u timestamp %llu", k, (unsigned long long)ts);
            }
        }
    }
    while (r_avshare_readMediaFrame(MEDIA_AUDIO, out, sizeof(out), &ts) > 0) {}
    printf("audio basic/wrap: %u frames\n", seq);
}

static void test_audio_overrun(void) {
    uint8_t buf[8192], out[8192];
    uint64_t ts;
    uint32_t queued = 0, dropped = 0;
    uint32_t first = 10000;

    // reader stalls: fill until the writer drops; drops must be whole frames
    for (uint32_t seq = first; seq < first + 100; seq++) {
        int len = make_adts(buf, payload_for(seq), seq);
        int ret = w_avshare_addAudioFrame(buf, len, seq);
        if (ret == 0) {
            CHECK(dropped == 0, "frame %u queued after a drop while still full", seq);
            queued++;
        } else {
            CHECK(ret == 1, "overrun returns 1, got %d", ret);
            dropped++;
        }
    }
    CHECK(queued > 20 && dropped > 0, "ring capacity: queued %u dropped %u", queued, dropped);

    // peek does not consume
    uint8_t hdr[16];
    int p = r_avshare_peekAudio(hdr, sizeof(hdr));
    CHECK(p == 16 && hdr[0] == 0xFF && memcmp(hdr + 7, &first, 4) == 0, "peek oldest frame header");

    // reader catches up: exactly the queued frames, in order, intact
    for (uint32_t k = 0; k < queued; k++) {
        int n = r_avshare_readMediaFrame(MEDIA_AUDIO, out, sizeof(out), &ts);
        CHECK(n > 0 && check_adts(out, n, first + k, payload_for(first + k)), "overrun frame %u", first + k);
    }
    CHECK(r_avshare_readMediaFrame(MEDIA_AUDIO, out, sizeof(out), &ts) == 0, "nothing after the queued frames");

    // and the pair keeps working afterwards
    for (uint32_t seq = 20000; seq < 20200; seq++) {
        int len = make_adts(buf, payload_for(seq), seq);
        CHECK(w_avshare_addAudioFrame(buf, len, seq) == 0, "post-overrun write %u", seq);
        int n = r_avshare_readMediaFrame(MEDIA_AUDIO, out, sizeof(out), &ts);
        CHECK(n > 0 && check_adts(out, n, seq, payload_for(seq)) && ts == seq, "post-overrun read %u", seq);
    }
    printf("audio overrun: queued %u, dropped %u (whole frames)\n", queued, dropped);
}

static void test_audio_reconnect(void) {
    uint8_t buf[8192], out[8192];
    uint64_t ts;

    // move the writer away from slot 0
    for (uint32_t seq = 0; seq < 17; seq++) {
        int len = make_adts(buf, 100, seq);
        w_avshare_addAudioFrame(buf, len, seq);
    }
    // reader leaves and comes back before the writer notices (no write in between)
    r_avshare_stopMedia(MEDIA_AUDIO);
    CHECK(r_avshare_startMedia(MEDIA_AUDIO) == 0, "reader restart");

    for (uint32_t seq = 30000; seq < 30100; seq++) {
        int len = make_adts(buf, payload_for(seq), seq);
        CHECK(w_avshare_addAudioFrame(buf, len, seq) == 0, "reconnect write %u", seq);
        int n = r_avshare_readMediaFrame(MEDIA_AUDIO, out, sizeof(out), &ts);
        CHECK(n > 0 && check_adts(out, n, seq, payload_for(seq)), "reconnect read %u (len %d)", seq, n);
    }

    // after a real disconnect the writer reports no reader and restarts at slot 0
    r_avshare_stopMedia(MEDIA_AUDIO);
    // the shm is detached by the reader now; writer still attached
    CHECK(w_avshare_addAudioFrame(buf, 100, 1) == -3, "no reader after stop");
    CHECK(r_avshare_startMedia(MEDIA_AUDIO) == 0, "reader start again");
    for (uint32_t seq = 40000; seq < 40010; seq++) {
        int len = make_adts(buf, 200, seq);
        CHECK(w_avshare_addAudioFrame(buf, len, seq) == 0, "write %u", seq);
    }
    for (uint32_t seq = 40000; seq < 40010; seq++) {
        int n = r_avshare_readMediaFrame(MEDIA_AUDIO, out, sizeof(out), &ts);
        CHECK(n > 0 && check_adts(out, n, seq, 200), "read %u", seq);
    }
    printf("audio reconnect: ok\n");
}

// video frame: start code + NAL header + pattern
static int make_video(uint8_t *buf, int len, uint32_t seq) {
    buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 1; buf[4] = 0x41;
    for (int i = 5; i < len; i++) buf[i] = (uint8_t)(seq + i * 13) | 0x04;
    memcpy(buf + 5, &seq, 4);
    return len;
}

static void test_video_partial(void) {
    static uint8_t buf[600 * 1024], out[1024 * 1024];
    uint64_t ts;

    CHECK(r_avshare_startMedia(MEDIA_VIDEO) == 0, "video reader start");
    CHECK(w_avshare_connected(MEDIA_VIDEO), "writer sees video reader");

    // normal: SPS frame + I frame (multi-slot) + P frames, read back one frame at a time
    for (uint32_t seq = 0; seq < 300; seq++) {
        int type = (seq % 30 == 0) ? FRAME_I : FRAME_P;
        int len = make_video(buf, (type == FRAME_I) ? 45000 : 9000 + (seq % 5) * 1000, seq);
        w_avshare_addvid(buf, len, type, 5000000ull + seq * 33333ull);
        int n = r_avshare_readMediaFrame(MEDIA_VIDEO, out, sizeof(out), &ts);
        uint32_t got = 0;
        if (n > 9) memcpy(&got, out + 5, 4);
        CHECK(n == len && got == seq && ts == 5000000ull + seq * 33333ull, "video frame %u (len %d/%d)", seq, n, len);
    }

    // overrun with multi-slot frames: the legacy writer leaves a partial frame and then drops
    // until the next I frame; the reader must drop the partial frame and resync
    uint32_t seq = 1000;
    for (; seq < 1030; seq++) {
        int len = make_video(buf, 25000, seq); // 3 slots each: ring (56) full after 18 frames
        w_avshare_addvid(buf, len, (seq == 1000) ? FRAME_I : FRAME_P, seq);
    }
    uint32_t lastGood = 0;
    int nFrames = 0;
    for (;;) {
        int n = r_avshare_readMediaFrame(MEDIA_VIDEO, out, sizeof(out), &ts);
        if (n <= 0) break;
        uint32_t got = 0;
        memcpy(&got, out + 5, 4);
        CHECK(n == 25000 && got == 1000 + (uint32_t)nFrames, "queued frame %d is %u len %d", nFrames, got, n);
        lastGood = got;
        nFrames++;
    }
    CHECK(nFrames == 18, "18 whole frames before the partial one, got %d (last %u)", nFrames, lastGood);
    // partial frame (2 of 3 slots) still pending until a new frame starts
    // next I frame resumes the stream
    int len = make_video(buf, 25000, 2000);
    w_avshare_addvid(buf, len, FRAME_I, 2000);
    int n = r_avshare_readMediaFrame(MEDIA_VIDEO, out, sizeof(out), &ts);
    uint32_t got = 0;
    if (n > 9) memcpy(&got, out + 5, 4);
    CHECK(n == 25000 && got == 2000, "resync on next I frame after partial frame (len %d, seq %u)", n, got);
    for (seq = 2001; seq < 2100; seq++) {
        len = make_video(buf, 12000, seq);
        w_avshare_addvid(buf, len, FRAME_P, seq);
        n = r_avshare_readMediaFrame(MEDIA_VIDEO, out, sizeof(out), &ts);
        got = 0;
        if (n > 9) memcpy(&got, out + 5, 4);
        CHECK(n == 12000 && got == seq, "after resync frame %u", seq);
    }
    r_avshare_stopMedia(MEDIA_VIDEO);
    printf("video partial-frame recovery: ok\n");
}

// I frames larger than what a partial frame leaves free (above half the ring): the legacy video
// writer gives up a frame when the ring is full and drops until the next I frame; if the reader
// kept every partial frame until a new frame start shows up, each I frame would stay partial
// behind the previous one and the video would never recover.
static void test_video_big_iframes(void) {
    static uint8_t buf[600 * 1024], out[1024 * 1024];
    uint64_t ts;
    const int big = 40 * 10240 - 100; // 40 of the 56 slots
    uint32_t seq = 5000;
    int n;

    w_avshare_connected(MEDIA_VIDEO); // the record main loop polls it: the writer starts at slot 0
    CHECK(r_avshare_startMedia(MEDIA_VIDEO) == 0, "video reader start");
    for (int i = 0; i < 10; i++, seq++) {
        w_avshare_addvid(buf, make_video(buf, 15000, seq), FRAME_P, seq); // 2 slots each, not read yet
    }
    w_avshare_addvid(buf, make_video(buf, big, seq), FRAME_I, seq); // only 36 slots free: partial
    seq++;
    for (int i = 0; i < 10; i++) {
        n = r_avshare_readMediaFrame(MEDIA_VIDEO, out, sizeof(out), &ts);
        CHECK(n == 15000, "queued P frame %d (len %d)", i, n);
    }

    // one I frame per GOP, the reader polling every millisecond in between (like LiveNalVideoSource)
    int got = 0, iFrames = 0;
    for (iFrames = 1; iFrames <= 6 && !got; iFrames++, seq++) {
        w_avshare_addvid(buf, make_video(buf, big, seq), FRAME_I, seq);
        for (int t = 0; t < 200 && !got; t++) {
            n = r_avshare_readMediaFrame(MEDIA_VIDEO, out, sizeof(out), &ts);
            if (n == big) {
                uint32_t s2 = 0;
                memcpy(&s2, out + 5, 4);
                CHECK(s2 == seq && ts == seq, "big I frame content %u/%u", s2, seq);
                got = 1;
            }
            usleep(1000);
        }
    }
    CHECK(got, "a 40-slot I frame got through (tried %d I frames)", iFrames - 1);
    for (int i = 0; i < 50; i++, seq++) {
        int len = (i % 15 == 0) ? big : 9000;
        w_avshare_addvid(buf, make_video(buf, len, seq), (i % 15 == 0) ? FRAME_I : FRAME_P, seq);
        n = r_avshare_readMediaFrame(MEDIA_VIDEO, out, sizeof(out), &ts);
        CHECK(n == len && ts == seq, "big-I stream frame %u (len %d/%d)", seq, n, len);
    }
    r_avshare_stopMedia(MEDIA_VIDEO);
    printf("video I frames above half the ring: recovered after %d I frame(s)\n", iFrames - 1);
}

static void test_legacy_video(void) {
    static uint8_t buf[64 * 1024], out[1024 * 1024];
    struct timeval tv;
    unsigned dur;

    // the audio-off path: avshare_start()/readFrame() still behave as before
    r_avshare_start();
    CHECK(w_avshare_connected(MEDIA_VIDEO), "legacy start connects video");
    int total = 0;
    for (uint32_t seq = 0; seq < 3; seq++) {
        int len = make_video(buf, 15000, seq);
        w_avshare_addvid(buf, len, FRAME_P, seq);
        total += len;
    }
    int n = r_avshare_readFrame(out, sizeof(out), &tv, &dur);
    CHECK(n == total, "legacy readFrame returns all queued bytes (%d/%d)", n, total);
    r_avshare_stop();
    CHECK(!w_avshare_connected(MEDIA_VIDEO), "legacy stop disconnects video");
    printf("legacy video path: ok\n");
}

// concurrent writer/reader threads with real-time-ish pacing jitter
static volatile bool gStop = false;
static uint32_t gWritten = 0, gWriteDrops = 0;

static void *writer_thread(void *arg) {
    uint8_t buf[8192];
    (void)arg;
    for (uint32_t seq = 0; seq < 200000 && !gStop; seq++) {
        int len = make_adts(buf, payload_for(seq), seq);
        int ret;
        while ((ret = w_avshare_addAudioFrame(buf, len, seq)) == 1) {
            gWriteDrops++;
            break; // drop like the recorder does
        }
        if (ret == 0) gWritten++;
        if ((seq & 63) == 0) usleep(50);
    }
    return NULL;
}

static void test_concurrent(void) {
    uint8_t out[8192];
    uint64_t ts;
    pthread_t th;
    uint32_t nRead = 0, last = 0;
    bool first = true;
    int bad = 0;

    CHECK(r_avshare_startMedia(MEDIA_AUDIO) == 0, "reader start");
    gStop = false;
    pthread_create(&th, NULL, writer_thread, NULL);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        int n = r_avshare_readMediaFrame(MEDIA_AUDIO, out, sizeof(out), &ts);
        if (n > 0) {
            uint32_t seq = (uint32_t)ts;
            if (!check_adts(out, n, seq, payload_for(seq))) bad++;
            if (!first && seq <= last) bad++;
            first = false;
            last = seq;
            nRead++;
            if (seq == 199999) break;
        } else {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (t1.tv_sec - t0.tv_sec > 20) break;
            if ((nRead & 7) == 0) sched_yield();
        }
    }
    gStop = true;
    pthread_join(th, NULL);
    while (r_avshare_readMediaFrame(MEDIA_AUDIO, out, sizeof(out), &ts) > 0) nRead++;
    CHECK(bad == 0, "concurrent: %d corrupt/out-of-order frames", bad);
    CHECK(nRead == gWritten, "concurrent: read %u of %u queued", nRead, gWritten);
    r_avshare_stopMedia(MEDIA_AUDIO);
    printf("concurrent: %u queued, %u read, %u dropped by writer (ring full), %d bad\n", gWritten, nRead, gWriteDrops, bad);
}

static void test_timestamp(void) {
    struct timespec mono;
    struct timeval now, tv;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    gettimeofday(&now, NULL);
    uint64_t ts = (uint64_t)mono.tv_sec * 1000000 + mono.tv_nsec / 1000;
    r_avshare_timestampToTimeval(ts, &tv);
    long long diff = ((long long)tv.tv_sec - now.tv_sec) * 1000000 + (tv.tv_usec - now.tv_usec);
    CHECK(llabs(diff) < 5000, "monotonic->wall clock conversion off by %lld us", diff);
    struct timeval tv2;
    r_avshare_timestampToTimeval(ts + 21333, &tv2);
    long long d2 = ((long long)tv2.tv_sec - tv.tv_sec) * 1000000 + (tv2.tv_usec - tv.tv_usec);
    CHECK(d2 == 21333, "conversion keeps intervals (%lld)", d2);
    printf("timestamp conversion: wall-clock error %lld us\n", diff);
}

// The writer trusts a connect flag only while some process holds the lock rtspLive takes on
// /tmp/live.pid (a process does not see its own locks): a child plays the running rtspLive.
static pid_t gLockHolder = -1;

static void reader_process_start(void) {
    int ready[2];
    if (pipe(ready) != 0) return;
    gLockHolder = fork();
    if (gLockHolder == 0) {
        int fd = open("/tmp/live.pid", O_RDWR | O_CREAT, 0644);
        struct flock fl;
        memset(&fl, 0, sizeof(fl));
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        fcntl(fd, F_SETLK, &fl);
        if (write(ready[1], "x", 1) != 1) _exit(1);
        for (;;) pause();
    }
    char c;
    if (read(ready[0], &c, 1) != 1) printf("lock holder failed\n");
    close(ready[0]);
    close(ready[1]);
}

static void reader_process_kill(void) {
    if (gLockHolder > 0) {
        kill(gLockHolder, SIGKILL);
        waitpid(gLockHolder, NULL, 0);
        gLockHolder = -1;
    }
}

// rtspLive killed with its connect flag set: the writer must stop trusting the flag
static void test_stale_flag(void) {
    CHECK(r_avshare_startMedia(MEDIA_AUDIO) == 0, "reader start");
    usleep(600000);
    CHECK(w_avshare_connected(MEDIA_AUDIO), "reader alive: connected");
    reader_process_kill();
    usleep(600000); // the writer re-checks the lock every 500 ms
    CHECK(!w_avshare_connected(MEDIA_AUDIO), "reader process gone, flag still set: not connected");
    reader_process_start();
    usleep(600000);
    CHECK(w_avshare_connected(MEDIA_AUDIO), "a reader process again: connected");
    r_avshare_stopMedia(MEDIA_AUDIO);
    CHECK(!w_avshare_connected(MEDIA_AUDIO), "disconnected");
    printf("stale connect flag of a dead reader: ignored\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    reader_process_start();
    // the record process creates the segment at boot (its init clears the connect flags)
    CHECK(w_avshare_init() == 0, "writer init");

    test_timestamp();
    test_audio_basic();
    test_audio_overrun();
    test_audio_reconnect();
    r_avshare_stopMedia(MEDIA_AUDIO);
    test_video_partial();
    test_video_big_iframes();
    test_legacy_video();
    test_concurrent();
    test_stale_flag();

    reader_process_kill();
    w_avshare_uninit();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
