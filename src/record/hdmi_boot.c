/* HDMI audio (EXPERIMENTAL, off by default): what is in effect for this boot. See hdmi_boot.h. */
#include "hdmi_boot.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <log/log.h>
#include <minIni.h>

#include "appmsg.h"
#include "soc_reg.h"

#ifdef HB_TEST /* utilities/probe/test/hdmi_boot_test.sh: stops or kills the process at these points */
void hb_test_checkpoint(int n);
#define HB_CHECKPOINT(n) hb_test_checkpoint(n)
#else
#define HB_CHECKPOINT(n) ((void)0)
#endif

#define HB_RECORD_MAX 1024 // a valid record is about 300 bytes
#define HB_N(a)       ((int)(sizeof(a) / sizeof((a)[0])))

static const char *const hb_pins_name[] = {"untouched", "stock", "audio", "other", "unknown"};
static const char *const hb_fault_name[] = {"", HDMI_AUDIO_FAULT_PINS, HDMI_AUDIO_FAULT_CHIP, HDMI_AUDIO_FAULT_PATH,
                                            HDMI_AUDIO_FAULT_PROBE};
static const char *const hb_rec_name[] = {"valid", "missing", "from another boot", "unreadable or inconsistent"};

const char *hdmi_boot_fault_text(hb_fault_t f) {
    return (f > HB_FAULT_NONE && (int)f < HB_N(hb_fault_name)) ? hb_fault_name[f] : NULL;
}

bool hdmi_boot_supported(void) {
#if defined(HDZGOGGLE) || defined(HDZGOGGLE2) || defined(HDZBOXPRO)
    // the official packages of all three ship a byte-identical device tree and sys_config (I2S0
    // pin group PH1-PH3) and the same rc.sh register writes (docs/hdmi-audio.md, section 2)
    return true;
#else
    return false; // a target whose files were not compared: nothing read or written
#endif
}

bool hdmi_boot_setting_is_1(const char *setting_ini, const char *key) {
    char v[8] = "";
    if (setting_ini == NULL || ini_gets("hdmi", key, "", v, (int)sizeof(v), setting_ini) <= 0)
        return false;
    return strcmp(v, "1") == 0; // "01", "true", "2", "1x", "" ... are all off
}

/* ---- the probe's GO marker ---- */
#define HB_MARKER_MAX 1024

// a whole small text file, NUL-terminated; -1: missing (errno ENOENT) or unreadable, -2: too long
static int hb_read_file(const char *path, char *buf, size_t size) {
    size_t len = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    for (;;) {
        ssize_t r = read(fd, buf + len, size - 1 - len);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        len += (size_t)r;
        if (len >= size - 1)
            break;
    }
    close(fd);
    buf[len] = 0;
    return len >= size - 1 ? -2 : (int)len;
}

// "key : value" lines (sys_info, cpuinfo): the value of `key` (spaces and tabs in the key ignored, the
// value's ends trimmed) if it is 1..64 letters and digits and not all zeros
static bool hb_id_from(const char *path, const char *key, char *id, size_t len) {
    char buf[8192];
    if (hb_read_file(path, buf, sizeof(buf)) < 0)
        return false;
    for (char *line = buf; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char *colon = strchr(line, ':');
        if (colon) {
            char k[64];
            size_t n = 0;
            for (const char *p = line; p < colon && n < sizeof(k) - 1; p++)
                if (*p != ' ' && *p != '\t')
                    k[n++] = *p;
            k[n] = 0;
            char *v = colon + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            size_t vl = strlen(v);
            while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\t' || v[vl - 1] == '\r'))
                v[--vl] = 0;
            if (strcmp(k, key) == 0) {
                bool ok = vl >= 1 && vl <= 64 && vl < len, zero = true;
                for (size_t i = 0; ok && i < vl; i++) {
                    const char c = v[i];
                    ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                    zero &= c == '0';
                }
                if (ok && !zero) {
                    memcpy(id, v, vl + 1);
                    return true;
                }
                return false; // (the first line of that key counts, as in the probe)
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    return false;
}

void hdmi_boot_board_id(char *id, size_t len) {
    if (!hb_id_from(HDMIA_BOARD_INFO_FILE, "sunxi_chipid", id, len) &&
        !hb_id_from(HDMIA_BOARD_INFO_FILE, "sunxi_serial", id, len) &&
        !hb_id_from(HDMIA_CPUINFO_FILE, "Serial", id, len))
        snprintf(id, len, "unknown");
}

hb_probe_t hdmi_boot_probe(char *why, size_t wlen) {
    char buf[HB_MARKER_MAX + 2], board[72], go[16] = "", mark[72] = "";
    char dummy[8];
    if (why == NULL) {
        why = dummy;
        wlen = sizeof(dummy);
    }
    const int len = hb_read_file(HDMIA_PROBE_GO_FILE, buf, sizeof(buf));
    if (len < 0 && len != -2 && errno == ENOENT) {
        snprintf(why, wlen, "no GO of the HDMI audio probe (%s)", HDMIA_PROBE_GO_FILE);
        return HB_PROBE_MISSING;
    }
    if (len <= 0 || buf[len - 1] != '\n' || strlen(buf) != (size_t)len) {
        snprintf(why, wlen, "the probe's marker is unreadable, empty, too long or cut short");
        return HB_PROBE_INVALID;
    }
    // key=value lines; hdmi_audio_probe and board exactly once each, every line printable
    int n_go = 0, n_board = 0;
    for (char *line = buf; *line;) {
        char *nl = strchr(line, '\n');
        *nl = 0;
        char *eq = strchr(line, '=');
        for (const char *p = line; *p; p++)
            if (*p < 0x20 || *p > 0x7e)
                eq = NULL;
        if (eq == NULL) {
            snprintf(why, wlen, "the probe's marker has a line that is not key=value");
            return HB_PROBE_INVALID;
        }
        *eq = 0;
        if (strcmp(line, "hdmi_audio_probe") == 0) {
            n_go++;
            snprintf(go, sizeof(go), "%.15s", eq + 1);
        } else if (strcmp(line, "board") == 0) {
            n_board++;
            snprintf(mark, sizeof(mark), "%.71s", eq + 1);
        }
        line = nl + 1;
    }
    if (n_go != 1 || n_board != 1) {
        snprintf(why, wlen, "the probe's marker lacks or repeats hdmi_audio_probe= / board=");
        return HB_PROBE_INVALID;
    }
    if (strcmp(go, "GO") != 0) {
        snprintf(why, wlen, "the HDMI audio probe said %s", go);
        return HB_PROBE_NOT_GO;
    }
    hdmi_boot_board_id(board, sizeof(board));
    if (strcmp(mark, board) != 0) {
        snprintf(why, wlen, "the probe's GO is for board %.20s, this is %.20s", mark, board);
        return HB_PROBE_OTHER_BOARD;
    }
    snprintf(why, wlen, "GO of the HDMI audio probe for board %.20s", board);
    return HB_PROBE_GO;
}

hb_pins_t hdmi_boot_ph_state(uint32_t v) {
    if ((v & ~HB_PH_OWNED_MASK) != (HB_PH_STOCK & ~HB_PH_OWNED_MASK))
        return HB_PINS_OTHER; // a field HDMI audio does not own is not as rc.sh leaves it
    if ((v & HB_PH_OWNED_MASK) == (HB_PH_STOCK & HB_PH_OWNED_MASK))
        return HB_PINS_STOCK;
    if ((v & HB_PH_OWNED_MASK) == HB_PH_OWNED_AUDIO)
        return HB_PINS_AUDIO;
    return HB_PINS_OTHER; // PH1-PH3 in neither known state
}

// the kernel's id of this boot (8-4-4-4-12 lower-case hex), "" if it cannot be read
static void hb_boot_id(char *id, size_t len) {
    char buf[64];
    id[0] = 0;
    int fd = open(HDMIA_BOOT_ID_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r < 36 || len < 37)
        return;
    for (int i = 0; i < 36; i++) {
        const char c = buf[i];
        const bool dash = (i == 8 || i == 13 || i == 18 || i == 23);
        if (dash ? (c != '-') : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return;
    }
    if (r > 36 && buf[36] != '\n')
        return;
    memcpy(id, buf, 36);
    id[36] = 0;
}

static void hb_detail(hdmi_boot_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void hb_detail(hdmi_boot_t *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b->detail, sizeof(b->detail), fmt, ap);
    va_end(ap);
    for (char *p = b->detail; *p; p++) // one printable line
        if (*p < 0x20 || *p > 0x7e)
            *p = '?';
}

int hdmi_boot_write(const hdmi_boot_t *b) {
    const char *tmp = HDMIA_BOOT_FILE ".tmp";
    char ph[16] = "";
    if (b->ph_valid)
        snprintf(ph, sizeof(ph), "0x%08x", b->ph_cfg0);
    if ((int)b->pins < 0 || (int)b->pins >= HB_N(hb_pins_name) || (int)b->fault < 0 || (int)b->fault >= HB_N(hb_fault_name))
        return -1;
    FILE *f = fopen(tmp, "w");
    if (f == NULL)
        return -1;
    const int n = fprintf(f, "version=1\nboot_id=%s\nphase=%s\nsupported=%d\naudio_out=%d\naudio_in=%d\npins=%s\nfault=%s\nph_cfg0=%s\ndetail=%s\n",
                          b->boot_id, b->phase == HB_PHASE_CHECK ? "check" : "done", b->supported, b->out, b->in,
                          hb_pins_name[b->pins], hb_fault_name[b->fault], ph, b->detail);
    const bool ok = n > 0 && fflush(f) == 0 && !ferror(f);
    if (fclose(f) != 0 || !ok || rename(tmp, HDMIA_BOOT_FILE) != 0) {
        unlink(tmp);
        return -1; // /tmp full or read-only: the previous record (if any) stays as it was
    }
    return 0;
}

/* ---- reading: every line must be there exactly once and check out ---- */
static int hb_bool(const char *v, bool *out) {
    if (strcmp(v, "0") == 0)
        *out = false;
    else if (strcmp(v, "1") == 0)
        *out = true;
    else
        return -1;
    return 0;
}

static int hb_name(const char *v, const char *const *names, int n) {
    for (int i = 0; i < n; i++)
        if (strcmp(v, names[i]) == 0)
            return i;
    return -1;
}

static bool hb_consistent(const hdmi_boot_t *b) {
    if (b->supported != hdmi_boot_supported())
        return false;
    if (!b->supported)
        return b->phase == HB_PHASE_DONE && !b->out && !b->in && b->pins == HB_PINS_UNTOUCHED && b->fault == HB_FAULT_NONE;
    if (b->phase == HB_PHASE_CHECK)
        return b->out && b->fault == HB_FAULT_NONE && b->pins == HB_PINS_UNKNOWN;
    if (!b->out)
        return b->pins == HB_PINS_UNTOUCHED && b->fault == HB_FAULT_NONE;
    switch (b->pins) {
    case HB_PINS_UNTOUCHED: // refused without the probe's GO: PH_CFG0 neither read nor written
        return !b->ph_valid && b->fault == HB_FAULT_PROBE;
    case HB_PINS_AUDIO: // switched; a later fault (chip, path) may have turned HDMI out audio off since
        return b->ph_valid && b->ph_cfg0 == HB_PH_AUDIO && b->fault != HB_FAULT_PINS && b->fault != HB_FAULT_PROBE;
    case HB_PINS_STOCK:
        return b->ph_valid && b->ph_cfg0 == HB_PH_STOCK && b->fault == HB_FAULT_PINS;
    case HB_PINS_OTHER:
        return b->ph_valid && hdmi_boot_ph_state(b->ph_cfg0) == HB_PINS_OTHER && b->fault == HB_FAULT_PINS;
    case HB_PINS_UNKNOWN:
        return b->fault == HB_FAULT_PINS;
    default:
        return false;
    }
}

hb_rec_t hdmi_boot_read(hdmi_boot_t *b) {
    enum { K_VERSION, K_BOOT_ID, K_PHASE, K_SUPPORTED, K_OUT, K_IN, K_PINS, K_FAULT, K_PH, K_DETAIL, K_NUM };
    static const char *const keys[K_NUM] = {"version", "boot_id", "phase", "supported", "audio_out",
                                            "audio_in", "pins", "fault", "ph_cfg0", "detail"};
    char buf[HB_RECORD_MAX + 2];
    size_t len = 0;
    unsigned seen = 0;

    memset(b, 0, sizeof(*b));
    int fd = open(HDMIA_BOOT_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? HB_REC_MISSING : HB_REC_CORRUPT;
    for (;;) { // the whole file at once: a record replaced meanwhile is seen old or new, never mixed
        ssize_t r = read(fd, buf + len, sizeof(buf) - 1 - len);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        len += (size_t)r;
        if (len >= sizeof(buf) - 1)
            break;
    }
    close(fd);
    if (len == 0 || len > HB_RECORD_MAX || buf[len - 1] != '\n')
        return HB_REC_CORRUPT; // empty, too long, or cut short
    buf[len] = 0;
    if (strlen(buf) != len)
        return HB_REC_CORRUPT; // a NUL byte inside

    for (char *line = buf; *line;) {
        char *nl = strchr(line, '\n');
        *nl = 0;
        char *eq = strchr(line, '=');
        if (eq == NULL)
            return HB_REC_CORRUPT;
        *eq = 0;
        const char *v = eq + 1;
        for (const char *p = v; *p; p++)
            if (*p < 0x20 || *p > 0x7e)
                return HB_REC_CORRUPT;
        const int k = hb_name(line, keys, K_NUM);
        if (k < 0 || (seen & (1u << k)))
            return HB_REC_CORRUPT; // an unknown or repeated line
        seen |= 1u << k;
        int i;
        switch (k) {
        case K_VERSION:
            if (strcmp(v, "1") != 0)
                return HB_REC_CORRUPT;
            break;
        case K_BOOT_ID:
            if (strlen(v) >= sizeof(b->boot_id))
                return HB_REC_CORRUPT;
            strcpy(b->boot_id, v);
            break;
        case K_PHASE:
            if (strcmp(v, "check") == 0)
                b->phase = HB_PHASE_CHECK;
            else if (strcmp(v, "done") == 0)
                b->phase = HB_PHASE_DONE;
            else
                return HB_REC_CORRUPT;
            break;
        case K_SUPPORTED:
            if (hb_bool(v, &b->supported) < 0)
                return HB_REC_CORRUPT;
            break;
        case K_OUT:
            if (hb_bool(v, &b->out) < 0)
                return HB_REC_CORRUPT;
            break;
        case K_IN:
            if (hb_bool(v, &b->in) < 0)
                return HB_REC_CORRUPT;
            break;
        case K_PINS:
            if ((i = hb_name(v, hb_pins_name, HB_N(hb_pins_name))) < 0)
                return HB_REC_CORRUPT;
            b->pins = (hb_pins_t)i;
            break;
        case K_FAULT:
            if ((i = hb_name(v, hb_fault_name, HB_N(hb_fault_name))) < 0)
                return HB_REC_CORRUPT;
            b->fault = (hb_fault_t)i;
            break;
        case K_PH:
            b->ph_valid = false;
            if (v[0] != 0) {
                uint32_t x = 0;
                if (strlen(v) != 10 || v[0] != '0' || v[1] != 'x')
                    return HB_REC_CORRUPT;
                for (i = 2; i < 10; i++) {
                    const char c = v[i];
                    if (c >= '0' && c <= '9')
                        x = (x << 4) | (uint32_t)(c - '0');
                    else if (c >= 'a' && c <= 'f')
                        x = (x << 4) | (uint32_t)(c - 'a' + 10);
                    else
                        return HB_REC_CORRUPT;
                }
                b->ph_cfg0 = x;
                b->ph_valid = true;
            }
            break;
        default: // K_DETAIL
            if (strlen(v) >= sizeof(b->detail))
                return HB_REC_CORRUPT;
            strcpy(b->detail, v);
            break;
        }
        line = nl + 1;
    }
    if (seen != (1u << K_NUM) - 1)
        return HB_REC_CORRUPT; // a line is missing
    if (!hb_consistent(b))
        return HB_REC_CORRUPT;
    char now[40];
    hb_boot_id(now, sizeof(now));
    if (strcmp(now, b->boot_id) != 0)
        return HB_REC_STALE; // written at another boot
    return HB_REC_OK;
}

static int hb_finish(hdmi_boot_t *b) {
    const int w = hdmi_boot_write(b);
    if (w != 0)
        LOGE("hdmi audio boot: %s not writable (%s)", HDMIA_BOOT_FILE, b->detail);
    else
        LOGI("hdmi audio boot: audio_out=%d audio_in=%d pins=%s%s%s: %s", b->out, b->in, hb_pins_name[b->pins],
             b->fault ? ", fault: " : "", hb_fault_name[b->fault], b->detail);
    return (w != 0 || b->fault != HB_FAULT_NONE) ? 1 : 0;
}

/* PH1-PH3 -> I2S0, with the strict rule (hdmi_boot.h). Sets pins, fault, ph_cfg0, detail. */
static void hb_switch(hdmi_boot_t *b) {
    char diag[160], d1[160];
    uint32_t v = 0;
    b->fault = HB_FAULT_PINS; // until the switch is verified
    b->pins = HB_PINS_UNKNOWN;
    b->ph_valid = false;
    const int lk = soc_reg_lock();
    if (lk < 0) {
        hb_detail(b, "sunxi_dump lock not available within 1 s: PH_CFG0 not read, not written");
        return;
    }
    if (soc_reg_read(HB_PH_CFG0, &v, diag, sizeof(diag)) < 0) {
        hb_detail(b, "PH_CFG0 read not trusted (%s): not written", diag);
        soc_reg_unlock(lk);
        return;
    }
    b->ph_cfg0 = v;
    b->ph_valid = true;
    switch (hdmi_boot_ph_state(v)) {
    case HB_PINS_AUDIO:
        b->pins = HB_PINS_AUDIO;
        b->fault = HB_FAULT_NONE;
        hb_detail(b, "PH_CFG0 0x%08x: PH1-PH3 are I2S0 already, not written", v);
        break;
    case HB_PINS_STOCK:
        HB_CHECKPOINT(2);
        (void)soc_reg_put(HB_PH_CFG0, HB_PH_AUDIO); // a constant, never built from what was read
        HB_CHECKPOINT(3);
        if (soc_reg_read(HB_PH_CFG0, &v, diag, sizeof(diag)) == 0 && v == HB_PH_AUDIO) {
            b->pins = HB_PINS_AUDIO;
            b->fault = HB_FAULT_NONE;
            b->ph_cfg0 = v;
            hb_detail(b, "PH_CFG0 0x%08x -> 0x%08x, verified: PH1-PH3 = I2S0 for this boot", HB_PH_STOCK, HB_PH_AUDIO);
            break;
        }
        // it did not read back as written: the stock value back, and check that
        snprintf(d1, sizeof(d1), "%s", diag);
        (void)soc_reg_put(HB_PH_CFG0, HB_PH_STOCK);
        HB_CHECKPOINT(4);
        if (soc_reg_read(HB_PH_CFG0, &v, diag, sizeof(diag)) == 0 && v == HB_PH_STOCK) {
            b->pins = HB_PINS_STOCK;
            b->ph_cfg0 = v;
            hb_detail(b, "PH_CFG0 0x%08x did not read back (%s): 0x%08x written back, verified", HB_PH_AUDIO, d1, HB_PH_STOCK);
        } else {
            b->pins = HB_PINS_UNKNOWN;
            b->ph_valid = false;
            hb_detail(b, "PH_CFG0 0x%08x did not read back (%s); stock 0x%08x written back, not verified (%s): power-cycle",
                      HB_PH_AUDIO, d1, HB_PH_STOCK, diag);
        }
        break;
    default:
        b->pins = HB_PINS_OTHER;
        hb_detail(b, "PH_CFG0 0x%08x is neither 0x%08x (rc.sh) nor 0x%08x: refused, not written", v, HB_PH_STOCK, HB_PH_AUDIO);
        break;
    }
    HB_CHECKPOINT(5);
    soc_reg_unlock(lk);
}

int hdmi_boot_run(const char *setting_ini) {
    hdmi_boot_t b;
    const hb_rec_t st = hdmi_boot_read(&b);
    if (st == HB_REC_OK) { // rc.sh runs once per boot; a second run (by hand) changes nothing
        LOGI("hdmi audio boot: already decided at this boot (%s): nothing done", b.detail);
        return 0;
    }
    memset(&b, 0, sizeof(b));
    hb_boot_id(b.boot_id, sizeof(b.boot_id));
    b.supported = hdmi_boot_supported();
    b.phase = HB_PHASE_DONE;
    b.pins = HB_PINS_UNTOUCHED;
    if (!b.supported) {
        hb_detail(&b, "HDMI audio is not supported on this target: nothing read or written");
        return hb_finish(&b);
    }
    b.out = hdmi_boot_setting_is_1(setting_ini, "audio_out");
    b.in = hdmi_boot_setting_is_1(setting_ini, "audio_in");
    if (!b.out) {
        hb_detail(&b, "audio_out is not 1: PH_CFG0 not read, not written");
        return hb_finish(&b);
    }
    // the read-only probe must have found this board's SoC side as expected (docs/hdmi-audio.md, 7)
    char why[96];
    if (hdmi_boot_probe(why, sizeof(why)) != HB_PROBE_GO) {
        b.fault = HB_FAULT_PROBE;
        hb_detail(&b, "run hdmi_audio_probe.sh first, PH_CFG0 not read or written: %s", why);
        return hb_finish(&b);
    }
    if (st == HB_REC_CORRUPT) { // a record nobody can date: do nothing this boot
        b.pins = HB_PINS_UNKNOWN;
        b.fault = HB_FAULT_PINS;
        hb_detail(&b, "a record was there already and unreadable: PH_CFG0 not read, not written");
        return hb_finish(&b);
    }
    // (a record from another boot is replaced: rc.sh has just written the stock value)
    // first the intent: if this process dies half way, the app sees phase=check and recovers
    b.phase = HB_PHASE_CHECK;
    b.pins = HB_PINS_UNKNOWN;
    hb_detail(&b, "boot step started");
    if (hdmi_boot_write(&b) != 0) {
        LOGE("hdmi audio boot: %s not writable: PH_CFG0 not read, not written; HDMI audio off for this boot", HDMIA_BOOT_FILE);
        return 1;
    }
    HB_CHECKPOINT(1);
    hb_switch(&b);
    b.phase = HB_PHASE_DONE;
    HB_CHECKPOINT(6);
    return hb_finish(&b);
}

/* The boot step was interrupted (phase=check): put PH1-PH3 back if they were switched, never
 * switch them; HDMI out audio stays off for this boot. */
static void hb_recover(hdmi_boot_t *b) {
    char diag[160];
    uint32_t v = 0;
    b->phase = HB_PHASE_DONE;
    b->fault = HB_FAULT_PINS;
    b->pins = HB_PINS_UNKNOWN;
    b->ph_valid = false;
    const int lk = soc_reg_lock();
    if (lk < 0) {
        hb_detail(b, "boot step interrupted; sunxi_dump lock not available: PH_CFG0 not checked");
    } else if (soc_reg_read(HB_PH_CFG0, &v, diag, sizeof(diag)) < 0) {
        hb_detail(b, "boot step interrupted; PH_CFG0 read not trusted (%s): not written", diag);
    } else if (hdmi_boot_ph_state(v) == HB_PINS_AUDIO) {
        (void)soc_reg_put(HB_PH_CFG0, HB_PH_STOCK);
        HB_CHECKPOINT(7);
        if (soc_reg_read(HB_PH_CFG0, &v, diag, sizeof(diag)) == 0 && v == HB_PH_STOCK) {
            b->pins = HB_PINS_STOCK;
            b->ph_cfg0 = v;
            b->ph_valid = true;
            hb_detail(b, "boot step interrupted: PH_CFG0 0x%08x -> 0x%08x (stock), verified", HB_PH_AUDIO, HB_PH_STOCK);
        } else {
            hb_detail(b, "boot step interrupted: stock 0x%08x written back, not verified (%s): power-cycle", HB_PH_STOCK, diag);
        }
    } else {
        b->pins = hdmi_boot_ph_state(v);
        b->ph_cfg0 = v;
        b->ph_valid = true;
        hb_detail(b, "boot step interrupted: PH_CFG0 0x%08x, not written", v);
    }
    soc_reg_unlock(lk);
    if (hdmi_boot_write(b) != 0)
        LOGE("hdmi audio: %s not writable (%s)", HDMIA_BOOT_FILE, b->detail);
    LOGE("hdmi audio: %s", b->detail);
}

void hdmi_boot_app_start(bool enabled_out, bool *out_active, bool *in_active, hb_fault_t *fault) {
    hdmi_boot_t b;
    char diag[160];
    uint32_t v = 0;
    *out_active = false;
    *in_active = false;
    *fault = HB_FAULT_NONE;
    if (!hdmi_boot_supported())
        return;
    const hb_rec_t st = hdmi_boot_read(&b);
    if (st != HB_REC_OK) {
        if (st != HB_REC_MISSING || enabled_out)
            LOGE("hdmi audio: no decision for this boot (%s %s): HDMI audio off", HDMIA_BOOT_FILE, hb_rec_name[st]);
        if (enabled_out)
            *fault = HB_FAULT_PINS;
        return;
    }
    if (b.out || b.in)
        LOGI("hdmi audio (experimental) this boot: audio_out=%d audio_in=%d pins=%s%s%s (%s)", b.out, b.in,
             hb_pins_name[b.pins], b.fault ? ", fault: " : "", hb_fault_name[b.fault], b.detail);
    *in_active = b.in;
    if (b.phase == HB_PHASE_CHECK) {
        hb_recover(&b);
        *fault = HB_FAULT_PINS;
        return;
    }
    if (!b.out)
        return;
    if (b.fault != HB_FAULT_NONE) {
        *fault = b.fault;
        return;
    }
    // PH1-PH3 = I2S0 since the boot step: still so? (read only)
    const int lk = soc_reg_lock();
    int r = -1;
    if (lk >= 0) {
        r = soc_reg_read(HB_PH_CFG0, &v, diag, sizeof(diag));
        soc_reg_unlock(lk);
    } else {
        snprintf(diag, sizeof(diag), "sunxi_dump lock not available within 1 s");
    }
    if (r == 0 && v == HB_PH_AUDIO) {
        *out_active = true;
        return;
    }
    b.fault = HB_FAULT_PINS;
    if (r == 0) {
        b.pins = hdmi_boot_ph_state(v);
        b.ph_cfg0 = v;
        b.ph_valid = true;
        hb_detail(&b, "at app start PH_CFG0 is 0x%08x, not 0x%08x: HDMI out audio off, not written", v, HB_PH_AUDIO);
    } else {
        b.pins = HB_PINS_UNKNOWN;
        b.ph_valid = false;
        hb_detail(&b, "at app start PH_CFG0 not checked (%s): HDMI out audio off", diag);
    }
    LOGE("hdmi audio: %s", b.detail);
    if (hdmi_boot_write(&b) != 0)
        LOGE("hdmi audio: %s not writable", HDMIA_BOOT_FILE);
    *fault = HB_FAULT_PINS;
}

void hdmi_boot_set_fault(hb_fault_t fault) {
    hdmi_boot_t b;
    if ((fault != HB_FAULT_CHIP && fault != HB_FAULT_PATH) || hdmi_boot_read(&b) != HB_REC_OK ||
        b.phase != HB_PHASE_DONE || !b.out || b.pins != HB_PINS_AUDIO || b.fault != HB_FAULT_NONE)
        return;
    b.fault = fault;
    hb_detail(&b, "%s: HDMI out audio off for the rest of this boot (PH1-PH3 stay I2S0 until the next boot)",
              hb_fault_name[fault]);
    if (hdmi_boot_write(&b) != 0)
        LOGE("hdmi audio: %s not writable", HDMIA_BOOT_FILE);
}

bool hdmi_boot_out_ready(void) {
    hdmi_boot_t b;
    return hdmi_boot_read(&b) == HB_REC_OK && b.phase == HB_PHASE_DONE && b.supported && b.out &&
           b.pins == HB_PINS_AUDIO && b.fault == HB_FAULT_NONE;
}
