#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void IT66121_close();
void IT66121_init();
void IT66121_set_phase(uint8_t phase, uint8_t inv);

/* ===== HDMI out audio (experimental, default off, see docs/hdmi-audio.md) =====
 * The audio setup (I2S, 2-ch L-PCM, 48 kHz, 16 bit in 32-bit slots) is part of IT66121_init()
 * when enabled, never a runtime toggle. Its Reg04 writes are constants that keep the video,
 * HDCP and RCLK resets as the stock init leaves them; a Reg04 read that fails or disagrees, or
 * a write that does not read back, puts the stock video-on value back and keeps audio off for
 * the rest of this boot (IT66121_audio_failed()). With audio not enabled IT66121_init() is
 * byte-identical to upstream (utilities/probe/test/it66121_seq_test.sh). */
void IT66121_audio_config(bool enable); // in effect for this boot: applied by every later IT66121_init()
bool IT66121_audio_failed(void);        // the Reg04 check tripped this boot ("HDMI chip check failed")
bool IT66121_is_active(void);           // between IT66121_init() and IT66121_close()
bool IT66121_audio_is_on(void);         // the last IT66121_init() enabled audio
/* ===== end HDMI out audio ===== */

#ifdef __cplusplus
}
#endif
