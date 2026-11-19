/* ===========================================================================
 * hdmi_audio_boot: the boot step of HDMI audio (EXPERIMENTAL, off by default).
 * rc.sh runs it once per boot, right after setup_uart() wrote the stock pin functions
 * and before the record daemon and the goggle app start. With setting.ini [hdmi]
 * audio_out other than exactly 1 (the default) it reads no register and writes none:
 * it only records "off" in /tmp/hdmi_audio.boot. See src/record/hdmi_boot.h and
 * docs/hdmi-audio.md.
 * Usage: hdmi_audio_boot [setting.ini]   (default /mnt/app/setting.ini)
 * Exit status 0 = nothing to report, 1 = HDMI audio stays off for this boot (see the record).
 * ======================================================================== */
#include "record/hdmi_boot.h"

int main(int argc, char *argv[]) {
    return hdmi_boot_run(argc > 1 ? argv[1] : "/mnt/app/setting.ini");
}
