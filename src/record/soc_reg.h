#pragma once
/* ===========================================================================
 * SoC registers through /sys/class/sunxi_dump (the BSP's dump_reg driver).
 *
 * A read goes through ONE address latch shared by every process: "echo addr > dump"
 * then "cat dump". The firmware's /usr/sbin/awr AND /usr/sbin/aww both use it (aww
 * writes, then reads back through the latch), so two users at the same time can read
 * each other's register. A write ("addr value" > write) does not use the latch.
 *
 * Every user in this firmware takes the same lock, HDMIA_LOCK_FILE (flock), around
 * its accesses: the HDMI audio boot step (hdmi_boot.c), the record daemon's HDMI audio
 * bridge (pcmbridge.c), the app's aww/awr calls (system_exec_sunxi_dump()), the module
 * bay (src/module/module.c) and the probe scripts (utilities/probe). Users that cannot
 * take it (see docs/hdmi-audio.md, "sunxi_dump users") are caught by the verified read:
 * three reads that must agree, each in the exact format of the driver.
 * ======================================================================== */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#define SOC_REG_LOCK_TRIES   20    // flock(LOCK_NB) attempts ...
#define SOC_REG_LOCK_WAIT_US 50000 // ... 50 ms apart: at most 1 s in all

// The lock: the fd to pass to soc_reg_unlock(), or -1 (held by another process for 1 s, or the
// lock file cannot be opened: /tmp read-only, full, or the name is not a file).
int soc_reg_lock(void);
int soc_reg_lock_n(int tries); // the same with 1..SOC_REG_LOCK_TRIES attempts (1 = do not wait)
void soc_reg_unlock(int fd);

// Verified read (caller holds the lock): three reads through the latch, each exactly "0x"
// followed by 1 to 8 hex digits and one newline (the driver prints "0x%08x\n"), all three
// equal. 0 = *val is set; -1 = failed, *val untouched. diag (may be NULL) gets the reason
// and the raw text for the log.
int soc_reg_read(uint32_t addr, uint32_t *val, char *diag, size_t diaglen);
// The write alone ("0x%08x 0x%08x" to the write node); 0 = the driver took it.
int soc_reg_put(uint32_t addr, uint32_t val);
// soc_reg_put(), then soc_reg_read() must return exactly val. 0 = verified.
int soc_reg_write(uint32_t addr, uint32_t val, char *diag, size_t diaglen);

#ifdef __cplusplus
}
#endif
