// Stands in for the app's DVR control in juricabi's firmware (src/core/dvr.c): publishes a config snapshot the way it
// does (a copy of the given file as /tmp/hdz-dvr-control/<pid>/record-<n>.conf, directory 0700, file 0600) and
// sends RECORD_CONFIG (or the command given) on ripples' record control queue (lib/log record_control.c); prints
// the recorder's answer.
// usage: ctl_conf <conf file> [n] [config|live_stop|pause]   exit 0 = the recorder accepted it
//   pause: what the app sends before a source switch (the recorder stops live video and waits);
//   live_stop: what it sends after one, with a snapshot (the recorder carries on)
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <log/record_control.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <conf file> [n] [config|live_stop|pause]\n", argv[0]);
        return 2;
    }
    const unsigned n = argc > 2 ? (unsigned)atoi(argv[2]) : 1u;
    const char *name = argc > 3 ? argv[3] : "config";
    unsigned command;
    if (strcmp(name, "config") == 0)
        command = RECORD_CONFIG;
    else if (strcmp(name, "live_stop") == 0)
        command = RECORD_LIVE_STOP;
    else if (strcmp(name, "pause") == 0)
        command = RECORD_PAUSE;
    else {
        fprintf(stderr, "unknown command %s\n", name);
        return 2;
    }
    char dir[64], path[RECORD_CONTROL_PATH];
    if (mkdir("/tmp/hdz-dvr-control", 0700) < 0 && errno != EEXIST) {
        perror("mkdir /tmp/hdz-dvr-control");
        return 1;
    }
    snprintf(dir, sizeof(dir), "/tmp/hdz-dvr-control/%u", (unsigned)getpid());
    if (mkdir(dir, 0700) < 0 && errno != EEXIST) {
        perror(dir);
        return 1;
    }
    snprintf(path, sizeof(path), "%s/record-%u.conf", dir, n);
    FILE *in = fopen(argv[1], "r"), *out = fopen(path, "w");
    if (in == NULL || out == NULL) {
        perror("snapshot copy");
        return 1;
    }
    char buf[4096];
    size_t len;
    while ((len = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, len, out) != len) {
            perror("snapshot write");
            return 1;
        }
    }
    fclose(in);
    if (fclose(out) != 0 || chmod(path, 0600) != 0) {
        perror("snapshot close");
        return 1;
    }

    const int q = record_control_open(false);
    if (q < 0) {
        perror("record_control_open");
        return 1;
    }
    record_control_request_t request = {.type = RECORD_CONTROL_TYPE, .magic = RECORD_CONTROL_MAGIC,
        .owner = (unsigned)getpid(), .cookie = ((unsigned)getpid() ^ (unsigned)time(NULL)) | 1u,
        .sequence = 1, .command = command};
    snprintf(request.config, sizeof(request.config), "%s", path);
    if (record_control_send(q, &request) != 0) {
        perror("record_control_send");
        return 1;
    }
    record_control_reply_t reply;
    for (int i = 0; i < 500; i++) {
        const int r = record_control_ack(q, &request, &reply);
        if (r < 0) {
            perror("record_control_ack");
            return 1;
        }
        if (r > 0) {
            printf("%s %s: error %d, recorder %u\n", name, path, (int)reply.error, (unsigned)reply.recorder);
            return reply.error != 0;
        }
        usleep(10000);
    }
    printf("%s %s: no answer within 5 s\n", name, path);
    return 1;
}
