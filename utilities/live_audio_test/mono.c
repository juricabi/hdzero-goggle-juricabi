// mono: prints CLOCK_MONOTONIC in microseconds, the clock of record_host's log (its "log clock: T0" line).
// t_kill.sh reads it right before it ends rtspLive, to put that moment on the recorder's log clock.
#include <stdio.h>
#include <time.h>

int main(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("%llu\n", (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000);
    return 0;
}
