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
} RecordMsgCommand_e;

/* HDMIA_LOCK_FILE  flock() around every /sys/class/sunxi_dump access (one address latch for all
 *                  processes): the app's aww / awr calls (system_exec_sunxi_dump()), the module bay
 *                  (src/module/module.c) and utilities/probe take it (src/record/soc_reg.h). */
#ifndef HDMIA_LOCK_FILE /* host tests put it elsewhere */
#define HDMIA_LOCK_FILE "/tmp/.sunxi_dump.lock"
#endif

/** @} */

#if defined (__cplusplus)
}
#endif
