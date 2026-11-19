// Send a goggle message to the record process (or to rtspLive), like the main app does.
// usage: gmsg record|live quit|start|stop|live_start|live_stop|aio_start|aio_stop|hdmia_start|hdmia_stop|<number>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>

#include "appmsg.h" // src/record: the record process' command numbers

#define GOGGLE_MSG_KEY 0x5680 // src/record/gogglemsg.c: key = 0x5680 + mtype - MTYPE_RECORD
#define MTYPE_LIVE_ID  3      // src/rtspLive/stream/appmsg.h

typedef struct { long mtype; long cmd; } CommandMsg_t;

int main(int argc, char **argv) {
    static const struct { const char *name; int cmd; } cmds[] = {
        {"quit", MSG_cmdQUIT}, {"start", MSG_cmdSTART}, {"stop", MSG_cmdSTOP},
        {"live_start", LIVE_cmdSTART}, {"live_stop", LIVE_cmdSTOP},
        {"aio_start", AIO_cmdSTART}, {"aio_stop", AIO_cmdSTOP},
        {"hdmia_start", HDMIA_cmdSTART}, {"hdmia_stop", HDMIA_cmdSTOP},
    };
    if (argc != 3) {
        fprintf(stderr, "usage: %s record|live <command>\n", argv[0]);
        return 2;
    }
    long mtype = (strcmp(argv[1], "live") == 0) ? MTYPE_LIVE_ID : MTYPE_RECORD;
    int cmd = atoi(argv[2]);
    for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        if (strcmp(argv[2], cmds[i].name) == 0) cmd = cmds[i].cmd;
    }
    int q = msgget(GOGGLE_MSG_KEY + mtype - MTYPE_RECORD, 0);
    if (q < 0) {
        perror("msgget");
        return 1;
    }
    CommandMsg_t m = {mtype, cmd};
    if (msgsnd(q, &m, sizeof(m) - sizeof(long), 0) != 0) {
        perror("msgsnd");
        return 1;
    }
    return 0;
}
