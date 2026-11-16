#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// "aww ..." / "awr ..." are run by system_exec_sunxi_dump()
int system_exec(const char *command);
int system_script(const char *script);

/* An awr / aww command (or anything else that reads through /sys/class/sunxi_dump: its one
 * address latch is shared by every process, see src/record/soc_reg.h). Runs it with system()
 * while holding the shared lock /tmp/.sunxi_dump.lock (non-blocking tries 50 ms apart, at most
 * 1 s). If the lock cannot be taken it still runs the command, as stock always did, and logs a
 * warning; after such a timeout the next calls within 10 s try the lock once without waiting
 * (a stuck holder costs at most one 1 s wait per 10 s). Logged like system_exec(), except
 * "awr ..." reads (polled up to 10 times a second for the analog RSSI; stock ran them with a
 * plain system() and no log line). Returns what system() returns; the emulator runs nothing. */
int system_exec_sunxi_dump(const char *command);

#ifdef __cplusplus
}
#endif
