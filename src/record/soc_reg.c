/* SoC registers through /sys/class/sunxi_dump: see soc_reg.h. */
#include "soc_reg.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "appmsg.h"

#define SOC_REG_DUMP  "/sys/class/sunxi_dump/dump"
#define SOC_REG_WRITE "/sys/class/sunxi_dump/write"

int soc_reg_lock_n(int tries) {
    if (tries < 1)
        tries = 1;
    if (tries > SOC_REG_LOCK_TRIES)
        tries = SOC_REG_LOCK_TRIES;
    int fd = open(HDMIA_LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    for (int i = 0; i < tries; i++) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0)
            return fd;
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
            break; // not a lock that is merely held (e.g. the file system does not support it)
        if (i + 1 < tries)
            usleep(SOC_REG_LOCK_WAIT_US);
    }
    close(fd);
    return -1;
}

int soc_reg_lock(void) {
    return soc_reg_lock_n(SOC_REG_LOCK_TRIES);
}

void soc_reg_unlock(int fd) {
    if (fd >= 0)
        close(fd); // releases the flock
}

static int soc_reg_node_write(const char *node, const char *text) { // the whole text in one write()
    const size_t len = strlen(text);
    int fd = open(node, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t r = write(fd, text, len);
    close(fd);
    return (r == (ssize_t)len) ? 0 : -1;
}

static void soc_reg_diag(char *diag, size_t diaglen, const char *fmt, const char *a, const char *b, const char *c) {
    if (diag != NULL && diaglen > 0)
        snprintf(diag, diaglen, fmt, a, b, c);
}

// one read through the latch; text gets what the driver answered (printable, for the log)
static int soc_reg_read1(uint32_t addr, uint32_t *val, char *text, size_t textlen) {
    char buf[48];
    snprintf(buf, sizeof(buf), "0x%08x", addr);
    snprintf(text, textlen, "(no answer)");
    if (soc_reg_node_write(SOC_REG_DUMP, buf) < 0)
        return -1;
    int fd = open(SOC_REG_DUMP, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = 0;
    size_t k = 0;
    for (ssize_t i = 0; i < n && k + 4 < textlen; i++) // printable copy for the log
        k += (size_t)snprintf(text + k, textlen - k, (buf[i] >= 0x20 && buf[i] < 0x7f) ? "%c" : "\\x%02x", (unsigned char)buf[i]);
    if (n == 0)
        snprintf(text, textlen, "(empty)");
    // exactly "0x" + 1..8 hex digits + "\n": nothing before, nothing after, not cut short
    if (n < 4 || buf[0] != '0' || buf[1] != 'x' || buf[n - 1] != '\n')
        return -1;
    const ssize_t digits = n - 3;
    if (digits < 1 || digits > 8)
        return -1;
    uint32_t v = 0;
    for (ssize_t i = 2; i < n - 1; i++) {
        const char ch = buf[i];
        uint32_t d;
        if (ch >= '0' && ch <= '9')
            d = (uint32_t)(ch - '0');
        else if (ch >= 'a' && ch <= 'f')
            d = (uint32_t)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F')
            d = (uint32_t)(ch - 'A' + 10);
        else
            return -1;
        v = (v << 4) | d;
    }
    *val = v;
    return 0;
}

int soc_reg_read(uint32_t addr, uint32_t *val, char *diag, size_t diaglen) {
    char t[3][40];
    uint32_t v[3] = {0, 0, 0};
    int bad = 0;
    for (int i = 0; i < 3; i++)
        if (soc_reg_read1(addr, &v[i], t[i], sizeof(t[i])) < 0)
            bad = 1;
    if (bad) {
        soc_reg_diag(diag, diaglen, "not the driver's format: '%s' '%s' '%s'", t[0], t[1], t[2]);
        return -1;
    }
    if (v[0] != v[1] || v[1] != v[2]) {
        soc_reg_diag(diag, diaglen, "three reads differ: %s %s %s (another latch user)", t[0], t[1], t[2]);
        return -1;
    }
    *val = v[0];
    if (diag != NULL && diaglen > 0)
        snprintf(diag, diaglen, "0x%08x", v[0]);
    return 0;
}

int soc_reg_put(uint32_t addr, uint32_t val) {
    char text[48];
    snprintf(text, sizeof(text), "0x%08x 0x%08x", addr, val);
    return soc_reg_node_write(SOC_REG_WRITE, text);
}

int soc_reg_write(uint32_t addr, uint32_t val, char *diag, size_t diaglen) {
    uint32_t r = 0;
    if (soc_reg_put(addr, val) < 0) {
        if (diag != NULL && diaglen > 0)
            snprintf(diag, diaglen, "write of 0x%08x not taken", val);
        return -1;
    }
    if (soc_reg_read(addr, &r, diag, diaglen) < 0)
        return -1;
    if (r != val) {
        if (diag != NULL && diaglen > 0)
            snprintf(diag, diaglen, "reads back 0x%08x, not 0x%08x", r, val);
        return -1;
    }
    return 0;
}
