#include "it66121.h"
#include "../core/common.hh"
#include "dm5680.h"
#include "driver/hardware.h"
#include "i2c.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <log/log.h>

uint8_t it66121_vi_phase = 0;

/* ===== HDMI out audio state (experimental, default off) ===== */
static bool it66121_audio_cfg_on = false; // HDMI out audio in effect for this boot (IT66121_audio_config())
static bool it66121_audio_fail = false;   // the Reg04 check tripped: audio stays off for the rest of this boot
static bool it66121_active = false;       // between IT66121_init() and IT66121_close()
static bool it66121_audio_on = false;     // audio block programmed and enabled by the last IT66121_init()
// HDMI audio: the audio code switches to register bank 1 and back. Every multi-register access
// to the chip (init, close, set_phase from the HDMI-in detect / ELRS / UI threads) holds this
// lock, so set_phase never writes Reg59/70/72 while bank 1 is selected (0x159/0x170/0x172).
static pthread_mutex_t it66121_lock = PTHREAD_MUTEX_INITIALIZER;
static int it66121_audio_enable(void);
static void it66121_audio_abort(void);
static void it66121_status_locked(char *buf, size_t len);
/* ===== end HDMI out audio state ===== */

void IT66121_close() {
    pthread_mutex_lock(&it66121_lock);
    it66121_active = false; // HDMI audio: the chip is put into reset below, audio setup is lost
    it66121_audio_on = false;

	#ifdef HDZGOGGLE2
        I2C_Write(ADDR_FPGA, 0x8a, 0x00);
	#endif
	
    DM5680_ResetHDMI_TX(0);
    pthread_mutex_unlock(&it66121_lock);
}

void IT66121_init() {
    pthread_mutex_lock(&it66121_lock);
	#ifdef HDZGOGGLE2
        I2C_Write(ADDR_FPGA, 0x8a, 0x01);
	#endif
	
    DM5680_ResetHDMI_TX(0);
    usleep(1000);
    DM5680_ResetHDMI_TX(1);
    usleep(10000);

    I2C_R_Write(ADDR_IT66121, 0x0f, 0x08);
    I2C_R_Write(ADDR_IT66121, 0x05, 0x60);
    I2C_R_Write(ADDR_IT66121, 0x61, 0x30);
    I2C_R_Write(ADDR_IT66121, 0x62, 0x80);
    I2C_R_Write(ADDR_IT66121, 0x64, 0x90);
    usleep(1000);

    I2C_R_Write(ADDR_IT66121, 0x62, 0x18);
    I2C_R_Write(ADDR_IT66121, 0x64, 0x1d);
    I2C_R_Write(ADDR_IT66121, 0x68, 0x10);
    I2C_R_Write(ADDR_IT66121, 0x04, 0x3c);
    I2C_R_Write(ADDR_IT66121, 0x04, 0x3d);
    usleep(1000);

    I2C_R_Write(ADDR_IT66121, 0x0f, 0x08);
    I2C_R_Write(ADDR_IT66121, 0xd1, 0x0c);
    I2C_R_Write(ADDR_IT66121, 0x71, 0x18);
    I2C_R_Write(ADDR_IT66121, 0x0f, 0x08);
    I2C_R_Write(ADDR_IT66121, 0xf8, 0xc3);
    I2C_R_Write(ADDR_IT66121, 0xf8, 0xa5);
    I2C_R_Write(ADDR_IT66121, 0x20, 0x88);
    I2C_R_Write(ADDR_IT66121, 0x37, 0x62);
    I2C_R_Write(ADDR_IT66121, 0x20, 0x08);
    I2C_R_Write(ADDR_IT66121, 0xf8, 0xff);
    I2C_R_Write(ADDR_IT66121, 0x59, 0x40);
    I2C_R_Write(ADDR_IT66121, 0xe1, 0x41);
    I2C_R_Write(ADDR_IT66121, 0x05, 0x60);
    I2C_R_Write(ADDR_IT66121, 0x0c, 0xff);
    I2C_R_Write(ADDR_IT66121, 0x0d, 0xff);
    I2C_R_Write(ADDR_IT66121, 0x0e, 0x4f);
    I2C_R_Write(ADDR_IT66121, 0x0c, 0x00);
    I2C_R_Write(ADDR_IT66121, 0x0d, 0x00);
    I2C_R_Write(ADDR_IT66121, 0x0e, 0x4c);
    I2C_R_Write(ADDR_IT66121, 0x09, 0xff);
    I2C_R_Write(ADDR_IT66121, 0x20, 0x08);
    I2C_R_Write(ADDR_IT66121, 0x6a, 0xff);
    I2C_R_Write(ADDR_IT66121, 0xc1, 0x01);
    I2C_R_Write(ADDR_IT66121, 0x72, 0x00);
    I2C_R_Write(ADDR_IT66121, 0x70, 0x48);
    I2C_R_Write(ADDR_IT66121, 0x0f, 0x08);
    I2C_R_Write(ADDR_IT66121, 0x72, 0x03);
    I2C_R_Write(ADDR_IT66121, 0x73, 0x00);
    I2C_R_Write(ADDR_IT66121, 0x74, 0x00);
    I2C_R_Write(ADDR_IT66121, 0x75, 0x00);
    I2C_R_Write(ADDR_IT66121, 0x76, 0x00);
    I2C_R_Write(ADDR_IT66121, 0x77, 0x08);
    I2C_R_Write(ADDR_IT66121, 0x78, 0x55);
    I2C_R_Write(ADDR_IT66121, 0x79, 0x3c);
    I2C_R_Write(ADDR_IT66121, 0x7a, 0x88);
    I2C_R_Write(ADDR_IT66121, 0x7b, 0x3e);
    I2C_R_Write(ADDR_IT66121, 0x7c, 0x00);
    I2C_R_Write(ADDR_IT66121, 0x7d, 0x08);
    I2C_R_Write(ADDR_IT66121, 0x7e, 0x51);
    I2C_R_Write(ADDR_IT66121, 0x7f, 0x0c);
    I2C_R_Write(ADDR_IT66121, 0x80, 0x00);
    I2C_R_Write(ADDR_IT66121, 0x81, 0x00);
    I2C_R_Write(ADDR_IT66121, 0x82, 0x00);
    I2C_R_Write(ADDR_IT66121, 0x83, 0x08);
    I2C_R_Write(ADDR_IT66121, 0x84, 0x00);
    I2C_R_Write(ADDR_IT66121, 0x85, 0x00);
    I2C_R_Write(ADDR_IT66121, 0x86, 0x84);
    I2C_R_Write(ADDR_IT66121, 0x87, 0x0e);
    I2C_R_Write(ADDR_IT66121, 0x88, 0x03);
    I2C_R_Write(ADDR_IT66121, 0x04, 0x14);
    I2C_R_Write(ADDR_IT66121, 0x90, 0x70);
    I2C_R_Write(ADDR_IT66121, 0x91, 0x89);
    I2C_R_Write(ADDR_IT66121, 0x92, 0xc0);
    I2C_R_Write(ADDR_IT66121, 0x93, 0x40);
    I2C_R_Write(ADDR_IT66121, 0x94, 0x80);
    I2C_R_Write(ADDR_IT66121, 0x95, 0x00);
    I2C_R_Write(ADDR_IT66121, 0x96, 0x2c);
    I2C_R_Write(ADDR_IT66121, 0x97, 0x00);
    I2C_R_Write(ADDR_IT66121, 0x98, 0x64);
    I2C_R_Write(ADDR_IT66121, 0x99, 0x04);
    I2C_R_Write(ADDR_IT66121, 0x9a, 0x28);
    I2C_R_Write(ADDR_IT66121, 0x9b, 0x60);
    I2C_R_Write(ADDR_IT66121, 0x9c, 0x40);
    I2C_R_Write(ADDR_IT66121, 0x9d, 0xff);
    I2C_R_Write(ADDR_IT66121, 0x9e, 0xff);
    I2C_R_Write(ADDR_IT66121, 0x9f, 0xff);
    I2C_R_Write(ADDR_IT66121, 0xa0, 0x00);
    I2C_R_Write(ADDR_IT66121, 0xa1, 0x50);
    I2C_R_Write(ADDR_IT66121, 0xa2, 0xff);
    I2C_R_Write(ADDR_IT66121, 0xa3, 0xff);
    I2C_R_Write(ADDR_IT66121, 0xa4, 0x4c);
    I2C_R_Write(ADDR_IT66121, 0xa5, 0x04);
    I2C_R_Write(ADDR_IT66121, 0xa6, 0xf0);
    I2C_R_Write(ADDR_IT66121, 0xb1, 0x00);
    I2C_R_Write(ADDR_IT66121, 0xb2, 0x00);
    I2C_R_Write(ADDR_IT66121, 0xc0, 0x01);
    I2C_R_Write(ADDR_IT66121, 0x4f, 0xf0);
    I2C_R_Write(ADDR_IT66121, 0x64, 0x94);
    I2C_R_Write(ADDR_IT66121, 0x68, 0x00);
    I2C_R_Write(ADDR_IT66121, 0x61, 0x02);
    I2C_R_Write(ADDR_IT66121, 0x63, 0x20);
    I2C_R_Write(ADDR_IT66121, 0x66, 0x17);
    I2C_R_Write(ADDR_IT66121, 0x65, 0x80);
    I2C_R_Write(ADDR_IT66121, 0x61, 0x02);
    I2C_R_Write(ADDR_IT66121, 0xc1, 0x00);
    I2C_R_Write(ADDR_IT66121, 0xc6, 0x03);

    /* HDMI audio (experimental): no extra register access unless HDMI out audio is in effect
       for this boot ([hdmi] audio_out=1 at boot and PH1-PH3 switched, src/record/hdmi_boot.h) */
    it66121_active = true;
    it66121_audio_on = false;
    if (it66121_audio_cfg_on && !it66121_audio_fail) {
        if (it66121_audio_enable() == 0) {
            char buf[160];
            it66121_audio_on = true;
            usleep(20000);
            it66121_status_locked(buf, sizeof(buf));
            LOGI("IT66121 audio on (I2S): %s", buf);
        } else {
            it66121_audio_fail = true; // off for the rest of this boot: the next inits are stock again
            it66121_audio_abort();
        }
    }
    pthread_mutex_unlock(&it66121_lock);
}

void IT66121_set_phase(uint8_t phase, uint8_t inv) {
    uint8_t rdat;

    pthread_mutex_lock(&it66121_lock);
    inv &= 1;
    I2C_R_Write(ADDR_IT66121, 0x59, 0x40 | (inv << 3));

    rdat = I2C_R_Read(ADDR_IT66121, 0x02);
    if (rdat == 0x12) {
        I2C_R_Write(ADDR_IT66121, 0x70, (0x48 | phase));
    } else if (rdat == 0x22) {
        I2C_R_Write(ADDR_IT66121, 0x72, (0x03 | (phase << 6)));
    }
    pthread_mutex_unlock(&it66121_lock);
}
/* ===========================================================================
 * HDMI out audio (EXPERIMENTAL, default off, see docs/hdmi-audio.md)
 *
 * Programs the IT66121 audio input for 2-channel LPCM, 48 kHz, 16 bit, I2S, at the end of
 * IT66121_init() and only when HDMI out audio is in effect for this boot. Never at runtime:
 * the Audio page only stores the setting.
 * References (docs/ folder):
 *   PG = IT66121_Programming_Guide.v1.05.pdf, chap. 7 "Program Audio Mode"
 *        p.31 (sequence), p.31-33 (N/CTS), p.33-35 (regE0-E5), p.36-37
 *        (channel status), chap. 9 p.46 (Audio InfoFrame, checksum formula).
 *   RL = IT66121_Register_List_Release_V1.0.pdf: Reg04/Reg0F p.3,
 *        Reg58-5F p.9, RegC0-C5 p.20, RegCE p.21, RegE0-E4 p.22-23,
 *        Reg130-137 p.25, Reg168-16D p.26-27, Reg191-199 p.29-30.
 *   Datasheet p.11: MCLK input is optional, I2S needs only SCK, WS, I2S0.
 * Bank 1 (Reg130-1BF) is selected with Reg0F[0]=1. IT66121_init() leaves
 * Reg0F = 0x08 (CEC clock gated, bank 0), so bank 1 is 0x09.
 * ======================================================================== */

#define IT66121_REG_BANK0() I2C_R_Write(ADDR_IT66121, 0x0f, 0x08)
#define IT66121_REG_BANK1() I2C_R_Write(ADDR_IT66121, 0x0f, 0x09)

/* Reg04 (RL p.3): bit7/6 reserved (test), bit5 RegSoftRefRst (RCLK reset), bit4 RegSoftARst
   (audio clock base reset), bit3 REGSoftVRst (video clock base reset), bit2 REGAudReset (audio
   FIFO reset), bit1 reserved, bit0 REGHDCP_rst (HDCP reset).
   The stock video init above (IT66121_init(), unchanged from upstream) writes 04=3c and 04=3d
   (everything in reset) and, as its last Reg04 write, 04=14: RCLK, video and HDCP resets
   released, both audio resets asserted. That is the stock video-on value.
   The audio path writes only the constants below: every bit except the two audio resets is
   exactly as the stock init leaves it, so no write of the audio path can assert a video, HDCP
   or RCLK reset, whatever a read returned. A read is only a precondition (two reads that
   agree, the expected state); every write is read back. */
#define IT66121_RST_AREF       0x10                                         // RegSoftARst
#define IT66121_RST_AUD        0x04                                         // REGAudReset
#define IT66121_R04_STOCK      0x14                                         // stock video-on value (see above)
#define IT66121_R04_KEEP       (IT66121_R04_STOCK & ~(IT66121_RST_AREF | IT66121_RST_AUD)) // 0x00
#define IT66121_R04_AREF_ON    (IT66121_R04_KEEP | IT66121_RST_AUD)         // 0x04: audio clock on, FIFO in reset
#define IT66121_R04_AUDIO_ON   (IT66121_R04_KEEP)                           // 0x00: both audio resets released
#define IT66121_R04_DEFINED    0x3d                                         // bits compared (5, 4, 3, 2, 0)
#define IT66121_R04_VIDEO_RSTS 0x29                                         // RCLK, video, HDCP resets
_Static_assert(((IT66121_R04_STOCK | IT66121_R04_AREF_ON | IT66121_R04_AUDIO_ON) & IT66121_R04_VIDEO_RSTS) == 0,
               "an IT66121 Reg04 value of the audio path asserts a video reset");

/* Audio InfoFrame payload (PG p.46, RL p.26): PB1 = CC (001: 2 channels, CT = 0 refer to stream),
   PB2 = SF/SS 0 (refer to stream header, required for L-PCM), PB4 = CA 0 (FL/FR), PB5 = 0. */
#define IT66121_AIF_PB1 0x01
#define IT66121_AIF_PB2 0x00
#define IT66121_AIF_PB4 0x00
#define IT66121_AIF_PB5 0x00

void IT66121_audio_config(bool enable) {
    it66121_audio_cfg_on = enable;
}

bool IT66121_audio_failed(void) {
    return it66121_audio_fail;
}

bool IT66121_is_active(void) {
    return it66121_active;
}

bool IT66121_audio_is_on(void) {
    return it66121_audio_on;
}

// Reg04 now: two reads that agree, else -1 (a failed transfer, or reads that differ)
static int it66121_r04_get(uint8_t *v) {
    uint8_t a = 0, b = 0;
    if (I2C_R_Read_checked(ADDR_IT66121, 0x04, &a) < 0 || I2C_R_Read_checked(ADDR_IT66121, 0x04, &b) < 0 || a != b)
        return -1;
    *v = a;
    return 0;
}

// Reg04 is in state want (its defined bits); *seen = the value read, for the log
static bool it66121_r04_is(uint8_t want, int *seen) {
    uint8_t v = 0;
    *seen = -1;
    if (it66121_r04_get(&v) < 0)
        return false;
    *seen = v;
    return (v & IT66121_R04_DEFINED) == (want & IT66121_R04_DEFINED);
}

// one step of the audio resets: Reg04 must be `from` now, gets the constant `to`, must read back
static int it66121_r04_step(uint8_t from, uint8_t to) {
    int seen;
    if (!it66121_r04_is(from, &seen)) {
        LOGE("IT66121 audio: Reg04 is %s0x%02x, expected 0x%02x: not written", seen < 0 ? "unreadable / unstable, " : "",
             seen < 0 ? 0 : seen, from);
        return -1;
    }
    I2C_R_Write(ADDR_IT66121, 0x04, to);
    if (!it66121_r04_is(to, &seen)) {
        LOGE("IT66121 audio: Reg04 0x%02x written, reads back %s0x%02x", to, seen < 0 ? "unreadable / unstable, " : "",
             seen < 0 ? 0 : seen);
        return -1;
    }
    return 0;
}

// any check failed: the stock video-on state (as IT66121_init() leaves it), audio off for this boot
static void it66121_audio_abort(void) {
    int seen;
    IT66121_REG_BANK0();
    I2C_R_Write(ADDR_IT66121, 0xe0, 0xc0); // audio sources off (reset default)
    I2C_R_Write(ADDR_IT66121, 0xce, 0x00); // no Audio InfoFrame
    I2C_R_Write(ADDR_IT66121, 0xe1, 0x41); // the value the stock init writes
    for (int i = 0; i < 2; i++) {
        I2C_R_Write(ADDR_IT66121, 0x04, IT66121_R04_STOCK);
        if (it66121_r04_is(IT66121_R04_STOCK, &seen)) {
            LOGE("IT66121 audio: check failed, Reg04 = 0x%02x (stock video on), verified; HDMI out audio off for this boot",
                 IT66121_R04_STOCK);
            return;
        }
    }
    LOGE("IT66121 audio: check failed, Reg04 0x%02x written, reads %s0x%02x; HDMI out audio off for this boot",
         IT66121_R04_STOCK, seen < 0 ? "unreadable / unstable, " : "", seen < 0 ? 0 : seen);
}

// 0 = audio programmed and enabled; -1 = a Reg04 check failed (the caller aborts)
static int it66121_audio_enable(void) {
    IT66121_REG_BANK0();

    // PG p.31 step 1: disable all audio sources. RegE0[7:6]=00: 16-bit word length, RegE0[4]=0: I2S.
    I2C_R_Write(ADDR_IT66121, 0xe0, 0x00);

    // PG p.31 step 2 (first half): release the audio clock base reset, keep the FIFO in reset while programming.
    if (it66121_r04_step(IT66121_R04_STOCK, IT66121_R04_AREF_ON) < 0)
        return -1;

    // RL p.9 Reg58: no external MCLK (REGMCLKSamp=0), auto over-sampling clock, auto IPCLK (= reset default 0x11).
    I2C_R_Write(ADDR_IT66121, 0x58, 0x11);

    // PG p.31-33 step 3: N = 6144 for 48 kHz (valid for every TMDS clock when the CTS is measured by hardware).
    IT66121_REG_BANK1();
    I2C_R_Write(ADDR_IT66121, 0x33, 0x00); // Reg133 N[7:0]
    I2C_R_Write(ADDR_IT66121, 0x34, 0x18); // Reg134 N[15:8]
    I2C_R_Write(ADDR_IT66121, 0x35, 0x00); // Reg135 N[19:16]
    IT66121_REG_BANK0();
    // RegC5[1] = 0 (CTS counted by hardware, PG p.31) is the reset default (RL p.20) and the stock
    // init never changes RegC5: not written (no read-modify-write of a password-protected register).

    // PG p.33-35 step 4: audio format, channel enable bits still 0.
    I2C_R_Write(ADDR_IT66121, 0xe0, 0x00); // 16 bit, I2S, sources off
    I2C_R_Write(ADDR_IT66121, 0xe1, 0x01); // one source: not full-packet; 32-bit-slot I2S, 1T delay, WS=0 left, MSB first
    I2C_R_Write(ADDR_IT66121, 0xe2, 0xe4); // FIFO n <- source n
    I2C_R_Write(ADDR_IT66121, 0xe3, 0x00); // no L/R swap, channel status from registers (RegE3[4]=0), no zero CTS
    I2C_R_Write(ADDR_IT66121, 0xe4, 0x00); // no flat (mute) bits, all samples valid
    I2C_R_Write(ADDR_IT66121, 0xe5, 0x00); // no HBR / DSD (PG p.35: RegE5 should be zero)

    // PG p.36-37 step 5: IEC60958 channel status (bank 1), consumer L-PCM, 48 kHz, 16 bit.
    IT66121_REG_BANK1();
    I2C_R_Write(ADDR_IT66121, 0x91, 0x00); // L-PCM, copyright asserted, no pre-emphasis
    I2C_R_Write(ADDR_IT66121, 0x92, 0x00); // category: general
    I2C_R_Write(ADDR_IT66121, 0x93, 0x01); // source number
    I2C_R_Write(ADDR_IT66121, 0x94, 0x02); // channel number (same values as the mainline Linux it66121 driver)
    I2C_R_Write(ADDR_IT66121, 0x98, 0x02); // clock accuracy level II, Fs = 48 kHz
    I2C_R_Write(ADDR_IT66121, 0x99, 0xd2); // original Fs = 48 kHz (1101), word length 16 bit (0010)

    // PG p.46: Audio InfoFrame, checksum = 0 - (Reg168 + Reg169 + Reg16B + Reg16C + 0x84 + 0x01 + 0x0A).
    I2C_R_Write(ADDR_IT66121, 0x68, IT66121_AIF_PB1);
    I2C_R_Write(ADDR_IT66121, 0x69, IT66121_AIF_PB2);
    I2C_R_Write(ADDR_IT66121, 0x6a, 0x00);
    I2C_R_Write(ADDR_IT66121, 0x6b, IT66121_AIF_PB4);
    I2C_R_Write(ADDR_IT66121, 0x6c, IT66121_AIF_PB5);
    I2C_R_Write(ADDR_IT66121, 0x6d, (uint8_t)(0x100 - ((IT66121_AIF_PB1 + IT66121_AIF_PB2 + IT66121_AIF_PB4 + IT66121_AIF_PB5 + 0x84 + 0x01 + 0x0a) & 0xff)));
    IT66121_REG_BANK0();
    I2C_R_Write(ADDR_IT66121, 0xce, 0x03); // RL p.21 RegCE: send Audio InfoFrame, repeat every field

    // PG p.31 step 2 (second half): release the audio FIFO reset.
    if (it66121_r04_step(IT66121_R04_AREF_ON, IT66121_R04_AUDIO_ON) < 0)
        return -1;

    // PG p.31 step 7: I2S, enable source 0 (pin I2S0, clocked by SCK/WS).
    I2C_R_Write(ADDR_IT66121, 0xe0, 0x01);
    return 0;
}

static uint32_t it66121_cts_locked(void) {
    uint8_t r135, r136, r137;

    if (!it66121_active)
        return 0;
    // RL p.25: Reg135[7:4] = CTSCnt[3:0], Reg136 = CTSCnt[11:4], Reg137 = CTSCnt[19:12] (read-only,
    // measured from the audio input clock). 0 = no audio clock at the chip, or not these registers
    // (PG p.31 names Reg1A0-1A2; both are logged until a device shows which one counts).
    IT66121_REG_BANK1();
    r135 = I2C_R_Read(ADDR_IT66121, 0x35);
    r136 = I2C_R_Read(ADDR_IT66121, 0x36);
    r137 = I2C_R_Read(ADDR_IT66121, 0x37);
    IT66121_REG_BANK0();
    return ((uint32_t)r137 << 12) | ((uint32_t)r136 << 4) | (r135 >> 4);
}

// one-line register summary for the log (reads only, apart from the bank register)
static void it66121_status_locked(char *buf, size_t len) {
    uint8_t r04, r5f, re0, rc0, rc1, rce, a0, a1, a2;
    uint32_t cts;

    if (!it66121_active) {
        snprintf(buf, len, "HDMI out off");
        return;
    }
    IT66121_REG_BANK0();
    r04 = I2C_R_Read(ADDR_IT66121, 0x04);
    r5f = I2C_R_Read(ADDR_IT66121, 0x5f);
    re0 = I2C_R_Read(ADDR_IT66121, 0xe0);
    rc0 = I2C_R_Read(ADDR_IT66121, 0xc0);
    rc1 = I2C_R_Read(ADDR_IT66121, 0xc1);
    rce = I2C_R_Read(ADDR_IT66121, 0xce);
    cts = it66121_cts_locked();
    IT66121_REG_BANK1();
    a0 = I2C_R_Read(ADDR_IT66121, 0xa0);
    a1 = I2C_R_Read(ADDR_IT66121, 0xa1);
    a2 = I2C_R_Read(ADDR_IT66121, 0xa2);
    IT66121_REG_BANK0();
    snprintf(buf, len, "04=%02x 5F=%02x E0=%02x C0=%02x C1=%02x CE=%02x CTS=%u (1A0-1A2=%02x %02x %02x)",
             r04, r5f, re0, rc0, rc1, rce, (unsigned)cts, a0, a1, a2);
}
/* ===== end HDMI out audio ===== */
