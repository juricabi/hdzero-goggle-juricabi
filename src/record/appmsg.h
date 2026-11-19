#pragma once

#if defined (__cplusplus)
extern "C" {
#endif

#define THIS_MTYPE  MTYPE_RECORD

/**
* @defgroup data structures

* This contains structure definitions
* @{
*/

typedef enum
{
    MTYPE_NONE   = 0,
    MTYPE_RECORD = 1,
    MTYPE_PLAYER = 2,
    MTYPE_BUT,
} GoggleMsgType_e;

typedef enum {
	MSG_none	 = 0,
	MSG_cmdQUIT  = 1,
	MSG_cmdSTART,
	MSG_cmdSTOP,

	MSG_cmdBUTT,
} GoggleMsgCommand_e;

typedef enum {
	LIVE_cmdSTART = MSG_cmdBUTT,
	LIVE_cmdSTOP,

	LIVE_cmdBUTT,
} LiveMsgCommand_e;

typedef enum {
	REC_cmdSAVE = LIVE_cmdBUTT,

	AIO_cmdSTART,
	AIO_cmdSTOP,

	AIO_cmdBUTT,

	/* HDMI audio bridge (experimental, src/record/pcmbridge.c); sent by src/core/hdmi_audio.c */
	HDMIA_cmdSTART = AIO_cmdBUTT, /* apply HDMIA_REQ_FILE */
	HDMIA_cmdSTOP,

	HDMIA_cmdBUTT,
} RecordMsgCommand_e;

/* HDMI audio bridge (experimental): files in /tmp (RAM, empty after every boot).
 * HDMIA_REQ_FILE   written by the app (src/core/hdmi_audio.c) before each command:
 *                  [hdmi_audio] seq= active= src= sink= sink2= i2s0_fix=
 * HDMIA_STATE_FILE written by the record daemon at start, after each command and at exit:
 *                  [hdmi_audio] state=running|stopped|failed seq= pid= err=
 *                  The app compares it with its request and re-sends (record daemon restarted
 *                  by the Storage page, queue not there yet, sink busy, ...).
 * HDMIA_LOCK_FILE  flock() around every /sys/class/sunxi_dump access (one address latch for all
 *                  processes): the app's aww / awr calls (system_exec_sunxi_dump()), the module bay
 *                  (src/module/module.c), the HDMI audio boot step and bridge and utilities/probe
 *                  take it (src/record/soc_reg.h).
 * HDMIA_BOOT_FILE  what is in effect for this boot: written by hdmi_audio_boot from rc.sh,
 *                  read by the app and the record daemon (src/record/hdmi_boot.h). */
#ifndef HDMIA_REQ_FILE /* host tests (utilities/probe/test/pcmbridge_test.sh) put them elsewhere */
#define HDMIA_REQ_FILE   "/tmp/hdmi_audio.req"
#define HDMIA_STATE_FILE "/tmp/hdmi_audio.state"
#endif
#ifndef HDMIA_LOCK_FILE /* host tests put it elsewhere */
#define HDMIA_LOCK_FILE "/tmp/.sunxi_dump.lock"
#endif
#ifndef HDMIA_BOOT_FILE /* host tests (utilities/probe/test/hdmi_boot_test.sh) put them elsewhere */
#define HDMIA_BOOT_FILE    "/tmp/hdmi_audio.boot"
#define HDMIA_BOOT_ID_FILE "/proc/sys/kernel/random/boot_id"
#endif
/* HDMIA_PROBE_GO_FILE  the read-only probe's GO for this board (utilities/probe/hdmi_audio_probe.sh writes
 *                      it on the SD card, and removes it on NO-GO): HDMI Out Audio needs it
 * HDMIA_BOARD_INFO_FILE, HDMIA_CPUINFO_FILE  where this board's id is read (src/record/hdmi_boot.c) */
#ifndef HDMIA_PROBE_GO_FILE /* host tests put them elsewhere */
#define HDMIA_PROBE_GO_FILE   "/mnt/extsd/hdmi_audio_go.txt"
#define HDMIA_BOARD_INFO_FILE "/sys/class/sunxi_info/sys_info"
#define HDMIA_CPUINFO_FILE    "/proc/cpuinfo"
#endif

/** @} */

#if defined (__cplusplus)
}
#endif
