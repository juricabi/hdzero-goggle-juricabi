# HDMI audio on the HDZero goggles - findings, experimental code, probe

Status: **experimental, everything default off.** Nothing here has been run on a
goggle yet; everything below marked as checked was checked on a PC (host tests against
models, the emulator). With the default `setting.ini` the firmware behaves as before
(host tests: `utilities/probe/test/it66121_seq_test.sh` - the IT66121 register sequence
is identical; `it66021_edid_test.sh` - the IT66021 I2C sequence and the EDID it serves
are identical; `hdmi_boot_test.sh` - no `sunxi_dump` access at all, so no pin function is
read or written). Switches: **Audio > HDMI Audio (experimental) > HDMI Out Audio / HDMI
In Audio** (`[hdmi] audio_out` / `audio_in`); both take effect at the next start
(section 6).

Applies to the original **HDZero Goggle** (`-DHDZ_GOGGLE=ON`), the **Goggle 2**
(`-DHDZ_GOGGLE2=ON`) and, since 2026-11-14, the **BoxPro** (`-DHDZ_BOXPRO=ON`): its
official package ships the Goggle 1's device tree and sys_config byte for byte and the
same `rc.sh` register writes (section 2), so it runs the same code with the same checks.
On all three the board wiring is unknown until the read-only probe has run on the device
(section 7): **HDMI audio on is NO-GO before that**, on the BoxPro as on the goggles.

**On `ripples-all`** (ripples' firmware 1.2.2 with this project's additions, docs/status.md) the
same code is ported from `all` (082b2f5): the boot step and its record, the IT66121 Reg04 guard, the
IT66021 EDID (merged with ripples' own EDID timings, whose checksum the 48 kHz-only EDID adjusts),
the bridge in ripples' recorder, the shared `/tmp/.sunxi_dump.lock`, the BoxPro included, all off
by default. Two differences until the Audio page's extension (docs/status.md, phase 3):

* **No switches on the Audio page yet** (ripples' page is unchanged): `[hdmi] audio_out` /
  `audio_in` are set by hand in `/mnt/app/setting.ini` (exactly `1`), then restart. No
  "Restart" / "Error" markers; the reason for a fault is in `HDZGOGGLE.log` and
  `/tmp/hdmi_audio.boot`.
* **HDMI In Audio** goes through ripples' codec routing, not `all`'s audio mixer: while HDMI in
  is shown the DAC is put into the output mixers and on the line out (`audio_sel.sh out_dac_on`,
  `out_on`, the analog line-in out of the mix) at ripples' **DVR Playback** volume; it is not
  recorded (no output mixer -> ADC path, no Record Headphone Mix). Leaving HDMI in puts the line
  out and the DAC back as stock HDMI in leaves them (off). `hdmi_audio_line_out()` in
  `src/core/hdmi_audio.c`; it is set again after a record daemon restart.

What is said below about the Audio page (its rows, markers and the mixer source) is `all`'s.

Sources: `PG`/`RL` = the IT66121 Programming Guide v1.05 / Register List (`docs/`); the
IT66121FN and IT66021FN datasheets; **V536 datasheet** = "V536-H/V526 Datasheet Revision
1.6" (Allwinner), chapters 11.1 (I2S/PCM) and 12.7 (PIO); **ITE driver** = ITE's public
IT6602/IT66021 driver `it6602.c` / `it6602_reg.h` (reyno123456/wireless_soc,
`AR8020SW/Driver/Board/Common/IT66021`); pkendall64's wiki page "Analog Module Backpack
Control".

## 1. Short answer

| | HDMI out audio (IT66121) | HDMI in audio (IT66021) |
|---|---|---|
| chip | 2-ch L-PCM via I2S (SCK, WS, I2S0; MCLK optional) or S/PDIF (datasheet) | outputs I2S (SCK, WS, I2S0, I2S1) / S/PDIF, MCLK (datasheet). ITE driver: Reg52[3:0] tri-states the I2S pins, Reg52[4] S/PDIF; the firmware writes Reg52 = 0x30, i.e. I2S pins driven, S/PDIF tri-stated |
| SoC side (V536 datasheet, shipped device tree) | I2S0 `daudio@0x05090000`; DT pin group `daudio0@0` = PH1/PH2/PH3 = I2S0 BCLK/LRCK/DOUT (PH_CFG0 function 3); `daudio_master=4`: the SoC drives BCLK/LRCK. ALSA card `snddaudio0`. The DT names no consumer | I2S2 `daudio@0x05092000`; `daudio2@0` = PG10/PG11/PG13 = I2S2 BCLK/LRCK/DIN (PG_CFG1 function 3); `daudio_master=1`: the external chip drives BCLK/LRCK. ALSA card `snddaudio2` |
| wiring on the board | **likely** to the IT66121 (a group of exactly a transmitter's three signals, the IT66121 the only I2S receiver); which SoC pin reaches which chip pin: **unknown** | **likely** to the IT66021 (a receiver's three signals; vendor note records from `hw:snddaudio2`); pin order at the chip: **unknown** |
| shipped firmware | audio never programmed (Reg04 audio resets stay asserted); **rc.sh re-muxes PH1 to uart4 and PH2/PH3 off at boot** | EDID advertises Basic Audio + L-PCM 2 ch 32/44.1/48 kHz; nothing captures I2S2 |
| what this branch adds (off by default, from the next start) | HDMI Out Audio (`[hdmi] audio_out=1`): PH1-PH3 switched to I2S0 once per boot by the rc.sh step `hdmi_audio_boot` (section 4a), IT66121 audio setup in `IT66121_init()` + the ADC capture (what is recorded) copied to I2S0 | HDMI In Audio (`[hdmi] audio_in=1`): I2S2 played into the codec DAC while HDMI in is shown, a source of the audio mixer: heard at the Audio page's HDMI In level, **recorded with Record Headphone Mix**; EDID offers 48 kHz L-PCM only |
| not possible | - | HDMI in -> HDMI out (the app switches HDMI out off while HDMI in is shown) |
| only the device can tell | whether PH1-PH3 reach SCK/WS/I2S0 of the IT66121 in this order (G1, G2 and BoxPro separately; on G2 also whether the FPGA is in the path), whether the TV plays the tone | whether the IT66021 drives SCK/WS by default, at which word clock and format |

Both goggles and the BoxPro ship **the same device tree and sys_config** (byte-identical),
so the expected answer is the same for all three; the board wiring and the Goggle-2-only
FPGA register 0x8A (written by `IT66121_init/close`) are the parts that can differ.

## 2. What the official OS images contain

Images: Goggle 1 `HDZEROGOGGLE_Rev20250319.zip` and Goggle 2 `HDZEROGOGGLE2_Rev20250715.zip`
from www.hd-zero.com, file `Recovery/HDZG_OS.bin` each.

* Layout: Allwinner PhoenixSuit image `IMAGEWTY` v3 (unencrypted, 34 parts). The runtime
  device tree is item `dtb` of `boot_package_nor.fex` (sunxi-package: scp, optee, u-boot,
  soc-cfg, dtb; 90512 bytes, identical to `sunxi.fex`); `sys_config.fex` is the text
  source; `boot.fex` is a legacy uImage holding an ARM zImage with an LZO payload
  (Linux 4.9.118, G1 build #43 2025-03-19, G2 build #69 2025-07-15); `rootfs.fex` is
  squashfs, `app.fex` jffs2 (= `/mnt/app`).
* G1 vs G2: `sunxi.fex`, `boot_package` dtb, `sys_config.fex`, `config.fex`, boot0 are
  **identical**. Kernels differ only in build; the set of sound-driver strings is identical.
  Root file systems are the same apart from rebuilt libraries. App partitions differ in
  HDZGOGGLE/record/rtspLive and update scripts; `script/rc.sh` is the same.
* No `IKCONFIG` in either kernel (so `/proc/config.gz` is expected to be absent). All sound
  drivers are built in (`/lib/modules/4.9.118` has only sunxi-wlan, vin, hdzero modules).
  No `/etc/asound.conf`: ALSA `default` = card 0.
* **BoxPro** (examined 2026-11-14): `HDZEROBOXPRO_Rev20250715.zip` (hd-zero.com/document,
  23,215,845 bytes, SHA-256
  `351397af9c7edb10d4f053948f0f81f56bd892b535d9611db22767460892419e`), file
  `Recovery/HDZB_OS.bin`, the same layout. Its `sys_config.fex` and `sunxi.fex` are
  **byte-identical** to the Goggle 1's (Rev20250319); the decompiled device trees do not
  differ. So daudio0 = PH1-PH3 (I2S0), daudio2 = PG10/PG11/PG13 (I2S2), the I2C buses and
  uart4 are those of the goggles. Its stock app (9.5.1) `rc.sh` makes the same register
  writes as the Goggle 1's (`setup_uart()`: PH_CFG0 = 0x35517751; `aww 0x050967c0`); the
  only difference is `dispw -s vdpo 720p60` instead of 1080p50. Not examined (and not in
  any file): the board wiring from PH1-PH3 to the IT66121 and from the IT66021 to
  PG10/PG11/PG13, as on the goggles. Inference, not checked on a BoxPro: at 720p60 (TMDS
  74.25 MHz, above the 27 MHz PG p.31 asks for) the IT66121 setup works unchanged, since
  N = 6144 with the hardware-counted CTS is valid at every TMDS clock (PG p.31-33).

### Sound devices in the device tree (both goggles and the BoxPro)

| DT node | compatible | parameters | ALSA card |
|---|---|---|---|
| `codec@0x05096000` + `i2s-controller@0x05096000`, `sound@0` | sunxi-internal-codec / -i2s, codec-machine | `aif2config=0`, `aif3config=0`, `gpio-spk` = PG12 | `audiocodec` (card 0, used by DVR and line out) |
| `daudio@0x05090000`, `sound@1` | sunxi-daudio, daudio0-machine | `daudio_master=4` (CBS_CFS: SoC drives BCLK/LRCK), `audio_format=1` (I2S), `signal_inversion=1`, `tdm_config=1` (I2S), `pcm_lrck_period=32`, `slot_width_select=32`, `word_select_size=16`, `mclk_div=0` (no MCLK), `tdm_num=0`, pins `daudio0@0` = **PH1 PH2 PH3** function `i2s0` (muxsel 3) | `snddaudio0` |
| `daudio@0x05091000`, `sound@2` | sunxi-tdmhdmi, sunxi-hdmi-machine | SoC-internal HDMI audio | none: the kernel has no `sunxi-hdmi-machine` driver |
| `daudio@0x05092000`, `sound@3` | sunxi-daudio, daudio2-machine | `daudio_master=1` (CBM_CFM: the external chip drives BCLK/LRCK), `audio_format=1`, `signal_inversion=0`, `tdm_config=1`, 32/32/16 as above, `tdm_num=2`, pins `daudio2@0` = **PG10 PG11 PG13** function `i2s2` | `snddaudio2` |
| `dmic-controller@0x05095000`, `sound@4` | dmic | `status = "disabled"` | - |

Card names come from the sunxi snddaudio machine driver (`"snddaudio"` + `tdm_num`).
The vendor notes in the repo use exactly `hw:audiocodec` and `hw:snddaudio2`.

### Pins

Functions per the V536 datasheet (PH_CFG0, PG_CFG0/1; the kernel's pinctrl table agrees):

| pin | function 3 (datasheet) | other | used for |
|---|---|---|---|
| PH0 | I2S0_MCLK | 5: UART4_TX | GPIO 224 = `GPIO_HDZ_RX_RESET` |
| PH1, PH2, PH3 | **I2S0_BCLK, I2S0_LRCK, I2S0_DOUT** | 2: AIF2, 5: UART4 RX/CTS/RTS | DT `daudio0@0` (I2S0) |
| PH4 | I2S0_DIN | 2: AIF2_DIN | GPIO 228 = `GPIO_HDZ_TX_RESET` |
| PG0 - PG5 | - | 2: SDC1 CLK/CMD/D0-D3 | DT `sdc1` (SDIO Wi-Fi, `[wlan] wlan_busnum=1`); PG0-PG2 = GPIO 192-194, the module-bay control of the firmware (`src/core/defines.h`, see below) |
| PG8, PG9 | PG9: I2S2_MCLK | | Wi-Fi `wlan_regon`, `wlan_hostwake` |
| PG10, PG11, PG13 | **I2S2_BCLK, I2S2_LRCK, I2S2_DIN** | 2: AIF3 | DT `daudio2@0` (I2S2) |
| PG12 | I2S2_DOUT | 2: AIF3_DOUT | codec `gpio-spk` (speaker amplifier enable) |

So the SoC side is established: the I2S0 group is BCLK, LRCK and DOUT (a transmitter), the
I2S2 group BCLK, LRCK and DIN (a receiver); the pins left out are exactly the ones the board
uses otherwise (two reset GPIOs, the speaker GPIO). That fits the ITE chips: IT66121 audio
inputs `SCK/MCLK`, `WS`, `I2S0..3` (IT66121FN datasheet p.6; "MCLK input is optional",
p.11), IT66021 outputs `SCK_MCLK`, `WS_SPDIF`, `I2S0`, `I2S1_3DR`, `MCLK_MUTE` (IT66021FN
datasheet p.5, p.11). That these pins are wired to these chips, and in which order, is not
documented: *likely*, not established.

**GPIO 192-194 (PG0-PG2), module bay.** The firmware drives them as module-bay CLK/DAT/CS
(`softspi`). Per pkendall64's wiki (original HDZero Goggle) they are on the connector
between the module-bay board and the goggle, and reach the 9-pin analog module connector
only after a 3-wire mod. Goggle 2: not documented. `utilities/probe/bay_gpio_pattern.sh`
finds them and their voltage.

**rc.sh pin overrides** (`mkapp/app/script/rc.sh`, `setup_uart()`, unchanged since the
first commit, same in both shipped images) write three PIO configuration registers raw,
after the drivers have set the pin mux:

| register | value | effect |
|---|---|---|
| `0x0300B098` PE_CFG2 | `0x00775577` | PE16/17 off (twi1), PE18/19 uart2, PE20/21 off |
| `0x0300B0D8` PG_CFG0 | `0x22777777` | **PG0-PG5 off** (sdc1 pins), PG6/7 uart1 |
| `0x0300B0FC` PH_CFG0 | `0x35517751` | PH0 out, **PH1 uart4, PH2/PH3 off**, PH4 out, PH5/6 uart3, PH7 uart0 |

So on a running goggle I2S0 is not on its pins. The DT also has `uart4` (`uart4@0` = PH0,
PH1, `uart_used=0`) competing with I2S0 for PH1; which driver owns PH1 is visible in
`pinmux-pins` (probe). HDMI Out Audio therefore needs PH1-PH3 back in function 3. PH_CFG0
also holds PH5/PH6 = UART3 (the ESP32 / ELRS backpack), PH7 = UART0 and the two reset
GPIO outputs PH0/PH4, so nothing in the firmware changes it at run time: the boot step of
section 4a switches PH1-PH3 at most once per boot, before the app and the record daemon
start, and only from the exact stock value to one constant. (The optional probe test
`hdmi_audio_test.sh`, started by hand, switches them for its tone by the same rule, with
the app paused, and puts the stock value back.)

**`/sys/class/sunxi_dump`** (what `awr`/`aww` use; the app runs `aww` from several places)
is one address latch for every process: `echo addr > dump; cat dump` (a single address
answers `0x%08x\n`, the value only: `dump_reg.c` of the BSP). Two users at the same time
can read each other's register; `aww` uses the latch too (it reads back what it wrote).
Writes (`/sys/class/sunxi_dump/write`) do not use the latch. Every user in this firmware
takes the lock file `/tmp/.sunxi_dump.lock` (flock, at most 1 s): the app's own `aww`/`awr`
calls (`system_exec_sunxi_dump()`, `src/util/system.c`; if the lock is not free within 1 s
the command runs anyway, as stock did, and the next 10 s do not wait again), the HDMI audio
boot step and bridge (`src/record/soc_reg.c`: three reads that must agree, each exactly in
the driver's format), the module bay (`read_reg()` in `src/module/module.c`) and the probe
kit (which also pauses the app). What cannot take it (an `awr` typed by hand) is caught by
the three agreeing reads.

### Other evidence

* `mkapp/app/app/record/audio/i2s/i2s-daudio-setup.txt`: enable daudio0 and daudio2;
  "must do: set i2s0 format to i2s (not pcm) manually: `aww 0x05090000 0x00060010`"
  = I2S0 CTL with BCLK_OUT (bit 18), LRCK_OUT (bit 17), MODE_SEL = 1 (I2S) (V536
  datasheet 11.1.5; reset value 0x00060000).
* `audiocodec/alsa-util/amixer-record.txt`: "I2S2: `arecord -Dhw:snddaudio2 ...`".
* Vendor experiment: commit `4392621` (2024-05-24, "update it66121 driver") added an
  `#else` variant of `IT66121_init()` that wrote IT66121 audio registers (bank 1
  `0x30-0x35` CTS/N, `0xC5`, `0x04=0x04`, `0xE0=0x01`, `0xE1=0x05`, `0xE2=0xE4`,
  channel status `0x191/0x193/0x198/0x199`, audio InfoFrame `0x168-0x16D` with checksum
  `0x71`); it was never enabled (`#if (1)`) and removed by `f6779a7` "delete dirty code".
  It did not enable the InfoFrame packet (RegCE), wrote RegC5 without the second
  password byte and selected "no data delay" (left-justified) framing.
* `I2S HUB FUNC` (mixer control of card `audiocodec`): in the Allwinner 4.9
  `sunxi-inter-i2s.c` it sets `HUB_EN` (bit 31) of the internal codec I2S FIFO control;
  `sunxi-daudio.c` has the matching `sunxi daudio audio hub mode` control. The audio hub
  shares a **playback** stream between I2S blocks; it gives no path from the capture to
  I2S0. Not used.
* Prebuilt `libmedia_mpp.so`: one AI device (`AI_DEV_MAX_NUM 1`), opened as ALSA
  `default` (codec); `audioHw_AO_Enable` opens `default`, or `hw:1,0` when
  `AIO_ATTR_S.mPcmCardId = PCM_CARD_TYPE_SNDHDMI`. `AI_devI2S = 3` accepted by
  `src/record/confparser.c` cannot work (one AI device).
  `AUDIO_FRAME_S.mLen` is the byte count of the whole interleaved frame (all channels),
  although `mm_comm_aio.h` calls it "data lenth per channel": `audioHw_AI_CapThread`
  (0x2c54c) sets it to the chunk bytes that `alsaSetPcmParams` computes as chunk frames x
  channels x physical width / 8 (0x2ad40-0x2ad88, the buffer one `snd_pcm_readi` fills),
  and `AIChannel_EmptyThisBuffer` copies `mLen` bytes. The bridge relies on that and logs
  the measured frame rate (`input delivers about N frames/s`; 24000 would mean otherwise).
* Tools on the goggle: busybox 1.27.2 (89 applets, no head/sort/find/printf/date/timeout;
  `flock` and `usleep` exist), i2c-tools (`i2cdetect/i2cdump/i2cget/i2cset`, `-r` ranges),
  alsa-utils (`amixer aplay arecord`), `awr`/`aww` (shell wrappers around
  `/sys/class/sunxi_dump/{dump,write}`), `gpior/gpiow`. Kernel has pinctrl/gpio debugfs.

## 3. IT66121: register sequence for 48 kHz / 16 bit / 2-ch L-PCM

Reg0F[0] selects the bank (RL p.3); `IT66121_init()` leaves Reg0F = 0x08 (CEC clock gated,
bank 0), so bank 1 (Reg130-1BF) is 0x09. Implemented in `src/driver/it66121.c`
(`it66121_audio_enable()`), applied at the end of every `IT66121_init()` (boot, and every
return from HDMI in) while HDMI out audio is in effect for this boot; never as a run-time
switch.
Every multi-register access of the driver (init, close, `IT66121_set_phase()` from the
HDMI-in detect / ELRS / UI threads, audio) holds one lock, so `set_phase` never writes
Reg59/70/72 while bank 1 is selected.

| step | write | meaning / reference |
|---|---|---|
| 1 | `0F=08`, `E0=00` | bank 0; all audio sources off, 16-bit word length, I2S (S/PDIF: `E0=10`). PG p.31 step 1, RL p.22 |
| 2a | `04=04` (only if two agreeing reads give `14`; read back) | release RegSoftARst (audio clock base); the FIFO reset (bit 2) stays on while programming. PG p.31 step 2, RL p.3. `IT66121_init()` ends with `04=14` (both audio resets on, video/HDCP/RCLK resets off) |
| clk | `58=11` | no external MCLK, auto over-sampling clock, auto IPCLK (reset default). RL p.9 |
| 3 | `0F=09`, `133=00 134=18 135=00`, `0F=08` | N = 6144 (48 kHz). PG p.31-33: valid for every TMDS clock when the CTS is counted by hardware |
| - | (RegC5 not written) | RegC5[1]=0 (hardware CTS, PG p.31) is the reset default (RL p.20) and the stock init never changes RegC5, a password-protected register: no read-modify-write of it |
| 4 | `E0=00 E1=01 E2=E4 E3=00 E4=00 E5=00` | format: one source, not full-packet; 32-bit-slot I2S with 1-bit delay, WS=0 left, MSB first (the SoC sends 16-bit samples in 32-bit slots, 64 fs); FIFO n <- source n; no swap; channel status from registers; no flat bits; no HBR. PG p.33-35, RL p.22-23 |
| 5 | `0F=09`, `191=00 192=00 193=01 194=02 198=02 199=D2` | IEC 60958 channel status: consumer L-PCM, category general, Fs 48 kHz, clock accuracy level II, original Fs 48 kHz, 16 bit (same values as the mainline Linux it66121 driver). PG p.36-37, RL p.29-30 |
| IF | `168=01 169=00 16A=00 16B=00 16C=00 16D=70`, `0F=08`, `CE=03` | Audio InfoFrame: CC = 2 channels, SF/SS = refer to stream (required for L-PCM), CA = FL/FR; checksum = 0 - (168+169+16B+16C+0x84+0x01+0x0A) = 0x70 (PG p.46); RegCE = send + repeat every field (RL p.21) |
| 2b | `04=00` (only if two agreeing reads give `04`; read back) | release the audio FIFO reset |
| 7 | `E0=01` | enable source 0 (I2S0 pin) |

**Reg04 guard.** Reg04 also holds the RCLK (bit 5), video (bit 3) and HDCP (bit 0) resets.
The audio path writes it only as the constants `04` and `00`, which keep those bits as the
stock init leaves them (checked at compile time), so no write of the audio path can put
HDMI out video into reset, whatever a read returned. A read is only a precondition: two
reads that agree (`i2c_read_checked()` reports a failed transfer, which `i2c_read()`
returns as 0) and give the expected state; every write is read back. On any mismatch the
stock video-on state is written back and verified (`E0=C0`, `CE=00`, `E1=41`, `04=14`),
audio stays off for the rest of the boot ("HDMI chip check failed" on the Audio page) and
every later `IT66121_init()` is upstream's sequence again. Host check
(`it66121_seq_test.sh`): every read failing in turn, every Reg04 value, reads that differ,
a write not taken and 3000 random runs per target: no Reg04 write with a video/HDCP/RCLK
reset bit.

There is no run-time off, mute or S/PDIF variant any more: switching HDMI Out Audio off
takes effect at the next start, whose `IT66121_init()` is the stock one. CTS read-back for
diagnosis (in the log line after the setup): Reg135[7:4]/136/137 (RL p.25); PG p.31 names
Reg1A0-1A2 instead - both are logged. A CTS of 0 means no audio clock was counted **in
these registers**: no clock at SCK/WS, a different pin order, the FPGA (G2), or the wrong
read-back register (section 8).

Relation to the video code: HDMI mode is already on (`C0=01`), AVMute cleared (`C1=00`),
GCP sent (`C6=03`) by `IT66121_init()`; audio needs TMDS > 27 MHz (PG p.31), the goggle
outputs 1080p/720p. `IT66121_set_phase()` only writes Reg59 (keeps RegNoAud2Mute = 0) and
Reg70/72 - no effect on audio. `IT66121_close()` holds the chip in reset (and on Goggle 2
writes FPGA reg 0x8A = 0), so audio is reprogrammed by the next `IT66121_init()`.
With audio off `IT66121_init()` issues the same 98 (Goggle, BoxPro) / 100 (Goggle 2) writes
as before; with audio on it adds 37 writes (32 of the setup, 5 bank selects of the status
line), 8 checked reads of Reg04 and a 20 ms wait.

## 4. SoC side of HDMI out: what is implemented

### 4a. Boot-time decision: PH1-PH3 once per boot

`rc.sh` runs `/mnt/app/app/hdmi_audio_boot` (`src/boot/hdmi_audio_boot.c`,
`src/record/hdmi_boot.c`) once per boot, right after `setup_uart()` wrote the stock
PH_CFG0 = 0x35517751 (same script and value on every target) and before the record daemon
or the app start. It decides what is in effect for **this boot** and writes the record
`/tmp/hdmi_audio.boot` (RAM: `version`, the kernel's `boot_id`, `phase` check/done,
`supported`, `audio_out`, `audio_in`, `pins`, `fault`, `ph_cfg0`, `detail`), which the app
and the record daemon read. Only `[hdmi] audio_out` exactly `1` makes it touch PH_CFG0, and
then by the strict rule: the whole register must read (three agreeing reads in the
driver's exact format, under the `sunxi_dump` lock) as 0x35517751 (stock) or 0x35513331
(only PH1-PH3 = function 3); the value written is one of these two constants, never built
from a read; after the write three agreeing reads must give 0x35513331, else the stock value
is written back and verified. Nothing switches the pins back during the boot (except the
recovery below); the next boot's `rc.sh` writes the stock value again, so Off + restart is
a full undo.

| situation | result |
|---|---|
| `audio_out` not exactly `1` (default; `01`, `true`, missing key, unreadable `setting.ini`) | no `sunxi_dump` access at all; record `pins=untouched`; HDMI out audio off |
| target | Goggle 1, Goggle 2 and BoxPro alike (same device tree, sys_config, rc.sh); a build for no known target: nothing read or written whatever `setting.ini` says |
| PH_CFG0 = 0x35517751 | 0x35513331 written and verified: in effect. Not verified: 0x35517751 written back and verified, fault "pin check failed" |
| PH_CFG0 = 0x35513331 already | not written; in effect |
| any other value (a field HDMI audio does not own not as `rc.sh` leaves it, PH1-PH3 in neither state) | refused, not written; fault "pin check failed" |
| reads that differ or are not in the driver's format, lock not free within 1 s, `/tmp` not writable | not written; HDMI out audio off ("pin check failed" where a record could be written) |
| a valid record of this boot exists (the step run again by hand) | nothing done |
| a record of another boot | replaced (rc.sh has just written the stock value) |
| a record nobody can read or date | nothing read or written this boot; fault |
| the step killed half way (record still `phase=check`) | the next app start puts PH_CFG0 back to stock if it is 0x35513331 (verified), never switches it; fault |
| app start, record says switched | PH_CFG0 read once (read only): still 0x35513331: in effect; else off, fault, not written |
| app start, record missing / stale / corrupt and `audio_out=1` | off, fault "pin check failed" |
| app restart, record daemon restart (SD check, format), an Audio page click | the switch is never repeated or undone |
| later fault: IT66121 Reg04 check, a route the record daemon refuses (`refused: ...`) or never confirms (6 tries) | HDMI out audio off for the rest of the boot, also across app restarts (written to the record); PH1-PH3 stay I2S0 until the next boot |

Host check: `utilities/probe/test/hdmi_boot_test.sh` (117 checks against a latch model
shared by forked processes, SIGKILL at every point of the boot step and of the recovery;
built and run for each of the three targets, plus a build for no known target).

### 4b. The bridge

The only digital audio the goggle has is the codec ADC capture (mic 1/2 or line-in, chosen
by `audio_sel.sh`), which the record daemon reads through MPP AI device 0 for the DVR.
The ALSA capture device is exclusive, so a second reader (e.g. `arecord | aplay`) would
break DVR audio.

`src/record/pcmbridge.c` therefore runs **inside the record daemon** and opens an extra
channel on the same MPP AI device (the DVR, `ai2aenc`, and `ai2ao` already share it this
way), so HDMI out carries **exactly the signal that is recorded**. Frames go to ALSA
`hw:snddaudio0` (S16_LE, 2 ch, 48 kHz, mono sources duplicated). A route to I2S0 is
refused ("refused: HDMI out audio is not in effect this boot", nothing opened or written)
unless the boot record says the pins were switched this boot with no fault. The bridge
never reads or writes a pin function. Before the first write it applies the vendor fix to
I2S0 CTL (MODE_SEL = I2S, BCLK/LRCK output; a register of the I2S0 block alone, which the
driver sets up at every open): read three times (must agree), reserved bits 0, written and
read back; a held lock, reads that differ, an implausible value or a write that does not
read back fail the start (the old value written back and verified). On stop - and on a
start that fails half-way - it puts back exactly these fields, if they still hold its own
values. Host check: `utilities/probe/test/pcmbridge_test.sh` (26 checks; PH_CFG0 is never
written in any run). The AI device is only
disabled when none of DVR, `ai2ao`, the WiFi stream audio and the bridge use it, also after
a failed start (one list of AI device users in `src/record/main.c`, checked in every
start/stop/exit order by `utilities/live_audio_test/t_aidev.sh`).

What HDMI out carries is the ADC capture, the same signal as the DVR and the WiFi stream:
the Audio page's Recording section decides it (Record Headphone Mix, Record Mic / Line In /
A/V In, Record Level; on `ripples-all` ripples' Audio Source, Mic Gain and Line/AV Gain), not
the headphone Volume.

Flow: HDMI Out Audio On (`setting.ini [hdmi] audio_out=1`, stored by the Audio page; by hand on
`ripples-all`) ->
restart -> the boot step (4a) -> `hdmi_audio_load()` at app start reads the record
(`hdmi_boot_app_start()`) and configures the IT66121 driver with what is in effect ->
`IT66121_init()` programs audio -> `hdmi_audio_apply(hdmi_in_shown)` (after
start-up and every source switch; the route depends on the source being switched **to**)
writes the request `/tmp/hdmi_audio.req` (`seq`, `active`, `src=ai`,
`sink=hw:snddaudio0`, `i2s0_fix=1`) and sends `HDMIA_cmdSTART` on the record daemon's
queue (key 0x5680). The daemon answers in `/tmp/hdmi_audio.state` (`state`, `seq`, `pid`,
`err`), also when it starts and exits. `hdmi_audio_poll()` (peripheral thread, about
every 0.5 s; no file access while the feature is off) re-sends what was not confirmed:
queue not there yet, sink missing or busy, record daemon restarted by the Storage page
(`formatsd.sh` / `chkfixsd.sh`) - after 3, 6, 12, 24, 30 s, six times, and again after a
daemon restart. Switching to HDMI in replaces it with the HDMI-in route (HDMI out is closed
there). A start of the HDMI-out route that the daemon refuses with a reason starting
`refused` (a check a retry will not pass this boot), or six unconfirmed tries, turn HDMI out
audio off for the rest of the boot ("audio path failed").

## 5. HDMI in audio

* **EDID** (`IT66021_edid()`). Shipped: Basic Audio (CEA byte 3 bit 6, by CEA-861 2-ch
  L-PCM at 32, 44.1 and 48 kHz) and one Short Audio Descriptor (0x8A-0x8C: L-PCM, 2 ch,
  32/44.1/48 kHz, 16/20/24 bit). With `audio_in=1` the EDID says only what the path does:
  Basic Audio cleared, SAD 48 kHz only (0x8B = 0x04). The chip serves the block checksums
  from RegC4/RegC5 in the mode the driver leaves it in (RegC0[1:0] = 00, shadow on; ITE
  driver `EDIDRAMInitial()` / `UpdateEDIDReg()`), so the adjusted block-1 checksum is
  written to RegC5 (and RAM byte 0xFF). Checked on the host (`it66021_edid_test.sh`: both
  checksums, CEA structure, audio claims; with `audio_in=0` identical to before).
  Trade-off: HDMI/CEA-861 expect a sink that takes any audio to take Basic Audio, so a
  48 kHz-only EDID is not strictly compliant; a source may then send no audio at all. One
  that sends another rate anyway is muted by the bridge, not played at the wrong speed.
* **IT66021 audio output.** The repository has no IT66021 register map, but the ITE driver
  documents the output control: Reg52 (`REG_RX_OUPT_CTRL1`) [3:0] `B_TriI2SIO`, [4]
  `B_TriSPDIF`, [5] `B_DisVAutoMute`; `IT6602AudioOutputEnable()` clears [4:0] to drive the
  pins. `IT66021_init()` writes Reg52 = 0x30: I2S pins driven, S/PDIF tri-stated, video
  auto-mute off. It also writes Reg7D (`REG_RX_HWMuteCtrl`) = 0x30, i.e. `B_HWMuteClr` and
  `B_HWAudMuteClrMode` set; the ITE driver pulses these (set, then clear) - what leaving
  them set does is unknown. Whether the chip outputs I2S with the right framing by
  default: device test (the probe dumps all three banks while HDMI in is active).
* **SoC**: `daudio2` is a clock slave; it only samples while the IT66021 drives SCK/WS. The
  I2S2 sample counter (RXCNT, `0x0509202C`) and a 10 s `arecord -D hw:snddaudio2` in
  `utilities/probe/hdmi_audio_test.sh` decide whether it does.

### HDMI-in audio as a mixer source: headphones and recording (HDMI In Audio = On)

**On `ripples-all`** (until the Audio page's extension, phase 3) this subsection is `all`'s
design. Here `hdmi_audio_line_out()` uses ripples' codec routing instead of the mixer: on, the
DAC switches (`DACL Mixer AIF1DA0L`, `DACR Mixer AIF1DA0R`), `audio_sel.sh out_linein_off`,
`out_dac_on`, `out_on`, then `dvr_set_dvr_audio_volume()`: the tone is heard on the line out at
the Audio page's **DVR Playback** volume, without the analog line in; off, `out_dac_off` and
`out_off` (what ripples' HDMI in leaves); set again after a record daemon restart. It is not
recorded (nothing switches the output mixer into the ADC), so the device test's step 2 does not
apply, and step 1's level is the DVR Playback volume.

While HDMI in is shown, the bridge plays the I2S2 capture (`hw:snddaudio2`) into the
internal codec's DAC (`plughw:audiocodec`, 48 kHz). From the DAC it enters the codec's
output mixers like any analog source:

```
I2S2 (IT66021) -> bridge -> AIF1 DA0 -> DAC -> L/R output mixer -+-> L+R sum -> lineout volume -> jack
                                                                 +-> "l/r_output mixer" -> ADC -> MPP AI -> DVR, stream
                                                                     (Record Headphone Mix)
```

`hdmi_audio_line_out()` in `src/core/hdmi_audio.c` is the one function that switches it,
and it only tells the audio mixer (`audio_mixer_set_live_dac()`, docs/audio.md "HDMI-in
audio"); the mixer alone writes the codec's controls. On (the route starts): the DAC is a
live source at the Audio page's **HDMI In** level (0.0 dB by default; the row is greyed
out while HDMI In Audio is not in effect), mixed with the analog sources at their levels
(mic + HDMI audio works), the line out stays on during HDMI in (Volume permitting), and
**Record Headphone Mix** records it with the rest of the headphone mix. The direct Record switches
cannot record it: there is no path from the DAC into the ADC except through the output
mixer. Off (before the route stops; it waits until the codec has it, so the DAC leaves the
mix before the bridge stops playing into it): HDMI in mutes the line out again, as stock.

Before this was joined to the mixer, the HDMI code ran its own amixer commands (DAC
switches, DAC volume 152, both ADC output-mixer switches, the line-in bypass off, line
out 31) and re-ran them after every record daemon restart, while the mixer's 2 s codec
watcher put its own state back: the two would have undone each other every 2 s. Now
the record daemon's libmedia_mpp reset is undone by the watcher alone, DAC source
included.

Coexistence and loops:
* The record daemon runs one bridge route at a time. While HDMI in is shown the app closes
  HDMI out, so the DVR -> HDMI out route is not needed then; the two routes are exclusive
  by the app's state, and switching replaces one with the other.
* The DVR capture (MPP AI on the codec ADC) keeps running next to the HDMI-in bridge: codec
  playback and capture run at the same time, but the codec's AIF1 has **one** sample-rate
  field for both (`sun8iw16-codec.c` `codec_hw_params`: `SYS_SR_CTRL` AIF1_FS; the machine
  driver sets PLL_AUDIO to 24.576 MHz for 48 kHz). Both run at 48 kHz (record.conf `[ai]
  sample_rate=48000`); the bridge refuses the route if the DVR is set to another rate.
* No loop: nothing plays the ADC capture back into the DAC while this route runs. The
  HDMI-out bridge (AI -> I2S0) only runs while HDMI out is open. `ai2ao` (AI -> AO = codec
  DAC, `AIO_cmdSTART`, not used by the app) would need the codec playback stream the
  bridge holds: its open fails (and the other way round). DVR playback in the app (also
  the codec DAC) happens in the menu, where this route is off.

Limits:
* An analog round trip DAC -> output mixer -> ADC: some noise; the ADC input gain is shared
  by all sources, and two loud sources mixed into the ADC can clip (nothing limits).
* Clock: I2S2 is clocked by the HDMI source, the codec by PLL_AUDIO. The bridge mutes until
  the incoming rate is seen to be 48 kHz (within 3 %) and mutes again if it is not; small
  drift is absorbed by dropping or repeating one frame per block (at most ~1400 ppm), an
  underrun is refilled with silence. Clicks at a correction are possible.
* Recorded only through the output-mixer -> ADC switches (Record Headphone Mix). The WiFi
  stream audio takes the same ADC capture as the DVR, so it carries it too; HDMI out is
  off while HDMI in is shown anyway.

Device test (HDMI In Audio On and one restart, and `RECORD.log` / `HDZGOGGLE.log` on the
SD card):
1. HDMI source playing a known tone on HDMI in, headphones on the jack, HDMI in selected:
   the tone is heard at the HDMI In level; `RECORD.log` shows `HDMI in audio at 48000 Hz:
   unmuted` and every 10 s `bridge: N frames in (~48000/s) ...`. A 44.1 kHz source:
   `muted`, silence.
2. Audio page: Record Headphone Mix Yes, Mic 0 dB. Start a DVR recording while the tone
   plays, speak into the mic: the file has the tone and the mic (once). Record Headphone Mix
   No: the file has only the direct Record sources, no tone. Leave HDMI in: line out goes
   off; switch to analog: A/V on the jack as before, no tone.
3. Storage page: repair or format the SD card while HDMI in plays: within ~10 s the
   `HDZGOGGLE.log` shows `record daemon restarted` and the tone is back.
4. Long run (10 min): count clicks; `drift-adjusted` in the bridge log should grow slowly.

* HDMI in -> HDMI out: not possible with the current app, `app_switch_to_hdmi_in()` calls
  `IT66121_close()`.

## 6. Switching it on (experimental)

On `ripples-all`, until the Audio page's extension (phase 3): no rows yet. Over SSH (WiFi
Module > System > SSH) put `audio_out=1` and/or `audio_in=1` under `[hdmi]` in
`/mnt/app/setting.ini` (exactly `1`; anything else is off) and restart; set `0` (or remove the
lines) and restart to switch off. What follows describes `all`'s rows; the rest (the boot step,
the checks, the reasons in the log) is the same.

**Audio > HDMI Audio (experimental)**, on the Goggle 1, the Goggle 2 and the BoxPro (the
paths are unproven on all three: leave both Off until the read-only probe has run on the
device). The switches only store the
setting (exactly `1` or `0`); nothing touches a pin function, I2S0 or the IT66121 because
one was clicked. Both take effect at the next start:

| Row | setting.ini | at the next start |
|---|---|---|
| HDMI Out Audio On / Off | `[hdmi] audio_out=1/0` | the boot step (4a) switches PH1-PH3 or leaves them stock; `IT66121_init()` programs audio or runs the stock sequence; the bridge route follows |
| HDMI In Audio On / Off | `[hdmi] audio_in=1/0` | the EDID (48 kHz L-PCM only; written when the IT66021 is set up at boot) and the HDMI-in route at every switch to HDMI in. A source that kept the old EDID and sends 44.1 kHz is muted by the bridge, not played wrong |

The page shows an orange **Restart** next to a switch whose position differs from what is in
effect this boot, and **Error** next to HDMI Out Audio when it was On at this start but
stayed off; its hint then reads "Off at this start: " and the reason: "pin check failed"
(the boot step refused or did not verify, or there is no valid record of this boot), "HDMI
chip check failed" (the Reg04 guard, section 3) or "audio path failed" (the record daemon
refused the route or never confirmed it). The keys can still be set by hand in
`/mnt/app/setting.ini` (then reboot). Logs: `/tmp/hdmi_audio.boot` (the decision of this
boot, its `detail` line says why); HDZGOGGLE log `hdmi audio (experimental): switches out=..
in=.., in effect this boot out=.. in=..`, `IT66121 audio on (I2S): 04=.. 5F=.. E0=01 ..
CTS=..` and `hdmi audio: route a -> b`; record daemon (`/mnt/extsd/RECORD.log` if that file exists)
lines tagged `pcmbridge` (`i2s0 fix: ...`, `first AI frame: len=...`,
`input delivers about N frames/s`, `bridge: N frames in ...`, `i2s0 undo: ...`).
`/tmp/hdmi_audio.req` / `/tmp/hdmi_audio.state` show the last request and the daemon's
answer. `record.conf [hdmi_audio] latency_ms` (default 60) stays tunable.

## 7. Probe procedure

See `utilities/probe/README.md`. In short: copy `hdmi_audio_probe.sh` to the SD card as
`develop.sh`, boot, wait for 2 beeps and 2 more minutes, send `hdmi_audio_probe_N.txt` and
`hdmi_audio_dmesg_N.txt`. Optional: `hdmi_audio_test.sh` (tone to HDMI out, 10 s HDMI in
recording) and `bay_gpio_pattern.sh` (module-bay pin finder, bay empty). Please do it on
both a Goggle 1 and a Goggle 2.

## 8. Decision table

| probe / test result | HDMI out audio | HDMI in audio |
|---|---|---|
| `/proc/asound/cards` lacks `snddaudio0` | daudio0 failed to probe (dmesg `daudio`/pinctrl lines): needs a kernel/DT change | - |
| `/proc/asound/cards` lacks `snddaudio2` | - | same for daudio2 |
| `pinmux-pins`: PH1 owned by the uart (5001000.uart) | the boot step's raw switch of PH_CFG0 is required (as implemented); a DT fix would be cleaner | - |
| probe: "PH_CFG0 state: NEITHER the stock value nor the I2S0 value" | the boot step refuses on this unit ("pin check failed"): report the value; the strict rule must not be loosened without knowing which field differs | - |
| probe: "/dev/mem and sunxi_dump DIFFER" or the latch bytes not `30 78` + 8 digits + `0a` | the latch is not what `soc_reg.c` expects: HDMI out audio stays off ("pin check failed"); report | - |
| Audio page "HDMI chip check failed" | Reg04 was not 0x14 after the stock init, or I2C reads failed: the IT66121 dump of the probe | - |
| Audio page "audio path failed" | `RECORD.log` (`pcmbridge` lines: the reason after `refused`) | - |
| test A: I2S0 TXCNT does not grow while `aplay` runs | I2S0 not clocking: MODE/master fix not applied (look at the CTL lines; "read not trusted" = nothing was written) | - |
| test A: TXCNT grows, Reg135-137 CTS stays 0 | no audio clock counted at the IT66121 **in these registers**. Equally possible: PH1-PH3 not wired to SCK/WS/I2S0; wired in another order (e.g. LRCK on SCK); on G2 the FPGA in the path; the CTS is read back in Reg1A0-1A2 (compare the logged 1A0-1A2). Next: the IT66121 bank dumps (probe part 2, test A) during the tone; a scope on PH1-PH3 and the IT66121 SCK/WS pins if possible; a test build with the I2S0 slot/format varied | - |
| test A: CTS (either register set) non-zero, ~148500 at 1080p, no sound on TV | clock arrives; check E0/CE in the log, try another TV; format (E1), channel status or InfoFrame | - |
| test A: tone heard | **works**: set `audio_out=1` and test the DVR path | - |
| test B: I2S2 RXCNT stays 0 / WAV only 44 bytes | - | no clock on PG10/PG11: the IT66021 does not drive SCK/WS (Reg52 = 0x30 drives the pins; the source may send no audio for this EDID; check the IT66021 bank dumps) or not wired |
| test B: RXCNT grows, WAV silent/noise | - | clocks arrive; data pin (PG13) or format mismatch: compare IT66021 banks with an audio-off dump |
| test B: WAV plays the source audio | - | **works**: `audio_in=1`; then the device test of section 5 |
| Goggle 1, Goggle 2 or BoxPro differ | wiring differs; keep per-target defaults (`#ifdef HDZGOGGLE2`, `HDZBOXPRO`) | same |

## 9. Established / likely / unknown

| statement | G1 | G2 |
|---|---|---|
| Kernel registers I2S0 (`snddaudio0`) and I2S2 (`snddaudio2`) cards; DT pins PH1-3 / PG10,11,13 | established (DT, kernel strings) | established (identical DT) |
| SoC pin functions: PH1/2/3 = I2S0 BCLK/LRCK/DOUT, PG10/11/13 = I2S2 BCLK/LRCK/DIN | established (V536 datasheet) | same |
| I2S0 is the SoC clock master, I2S2 a clock slave (DT `daudio_master`) | established (DT) | same |
| IT66121 needs only SCK/WS/I2S0, MCLK optional; register sequence above | established (datasheet, PG, RL) | same |
| shipped app never enables IT66121 audio; rc.sh moves PH1-3 off I2S0 | established | established |
| PH1-PH3 wired to the IT66121 SCK/WS/I2S0, in this order | likely (wiring), order unknown | likely, order unknown; whether the audio passes the FPGA (reg 0x8A) is unknown |
| PG10/11/13 wired to the IT66021 SCK/WS/I2S0 | likely | likely |
| IT66021 I2S pins driven (Reg52 = 0x30 per the ITE driver's bit names) | likely (ITE driver for the same family) | same |
| IT66021 outputs I2S audio with the right framing without extra setup | unknown | unknown |
| `I2S HUB FUNC` can feed HDMI from the capture path | no (playback-only mechanism) | no |
| PG0-PG5: DT sdc1/SDIO Wi-Fi pins, "IO disable" after rc.sh | established (DT, rc.sh) | same |
| GPIO 192-194 = the firmware's module-bay CLK/DAT/CS | established (`src/core/defines.h`) | same |
| ... on the goggle-side connector of the bay board; on the 9-pin module connector only after a 3-wire mod; I/O voltage | likely (pkendall64's wiki); voltage unknown | unknown (wiki covers the original goggle) |
| `AUDIO_FRAME_S.mLen` = whole interleaved frame in bytes | established (libmedia_mpp disassembly); logged on the device | same |

BoxPro: the rows established from the device tree, the datasheets and `rc.sh` hold as for
G1 (identical device tree and sys_config, the same `rc.sh` writes, section 2); the wiring
rows are only likely by analogy with the goggles (an inference: nobody has looked at its
board), the order unknown, and whether anything sits between the SoC and the IT66121
unknown. It has no module bay.
