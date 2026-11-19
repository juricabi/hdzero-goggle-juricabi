// Stands in for the app's DVR control on ripples-all (src/core/dvr.c): publishes a config snapshot the way it
// does (a copy of the given file as /tmp/hdz-dvr-control/<pid>/record-<n>.conf, directory 0700, file 0600) and
// sends RECORD_CONFIG on ripples' record control queue (lib/log record_control.c); prints the recorder's answer.
// usage: ctl_conf <conf file> [n]      exit 0 = the recorder accepted the snapshot
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
        fprintf(stderr, "usage: %s <conf file> [n]\n", argv[0]);
        return 2;
    }
    const unsigned n = argc > 2 ? (unsigned)atoi(argv[2]) : 1u;
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
        .sequence = 1, .command = RECORD_CONFIG};
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
            printf("CONFIG %s: error %d, recorder %u\n", path, (int)reply.error, (unsigned)reply.recorder);
            return reply.error != 0;
        }
        usleep(10000);
    }
    printf("CONFIG %s: no answer within 5 s\n", path);
    return 1;
}
