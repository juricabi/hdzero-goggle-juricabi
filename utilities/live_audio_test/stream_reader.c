// Stands in for rtspLive's audio track, without a server and a client: holds /tmp/live.pid
// locked like rtspLive does (the record process trusts the connect byte only while it is
// locked), connects the audio side of the ring with the real reader code
// (src/rtspLive/stream/avshare.c), reads and counts frames, and disconnects on SIGTERM/SIGINT.
// For the AI device test (t_aidev.sh): "stream on" = start this, "stream off" = stop it.
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "avshare.h"
#include "live_context.h"

static volatile sig_atomic_t gStop;

static void on_signal(int sig) {
    (void)sig;
    gStop = 1;
}

int main(void) {
    static uint8_t buf[64 * 1024];
    struct flock fl;
    int fd = open(LOCKFILE, O_RDWR | O_CREAT, 0644);
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fd < 0 || fcntl(fd, F_SETLK, &fl) != 0) {
        fprintf(stderr, "stream_reader: cannot lock %s (another reader running?)\n", LOCKFILE);
        return 1;
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (avshare_startMedia(MEDIA_AUDIO) != 0) {
        fprintf(stderr, "stream_reader: ring not available\n");
        return 1;
    }
    printf("stream_reader: audio connected\n");
    unsigned long frames = 0;
    while (!gStop) {
        uint64_t ts;
        int n = avshare_readMediaFrame(MEDIA_AUDIO, buf, sizeof(buf), &ts);
        if (n > 0)
            frames++;
        else
            usleep(5000);
    }
    avshare_stopMedia(MEDIA_AUDIO);
    printf("stream_reader: audio disconnected, %lu frames read\n", frames);
    close(fd); // releases the lock
    return 0;
}
