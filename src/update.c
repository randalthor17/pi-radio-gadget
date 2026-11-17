#include "update.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#include "log.h"

#define TAG "update"

static void fail(struct upd *u, int err, const char *what)
{
    snprintf(u->last, sizeof u->last, "error: %s", what);
    LOGW(TAG, "%s", what);
    if (u->fd >= 0) {
        close(u->fd);
        u->fd = -1;
        unlink(u->tmp);
    }
    u->state = UPD_IDLE;
    (void)err;
}

void upd_init(struct upd *u, const char *target, void (*restart)(void))
{
    memset(u, 0, sizeof *u);
    u->fd = -1;
    snprintf(u->target, sizeof u->target, "%s", target);
    snprintf(u->tmp, sizeof u->tmp, "%s.new", target);
    snprintf(u->prev, sizeof u->prev, "%s.prev", target);
    const char *slash = strrchr(target, '/');
    int dir = slash ? (int)(slash - target) : 0;
    snprintf(u->pending, sizeof u->pending, "%.*s%spending", dir, target, slash ? "/" : "");
    u->restart = restart;
    snprintf(u->disable, sizeof u->disable, "%s", "/boot/firmware/pi-radiogadget.disable");
    snprintf(u->last, sizeof u->last, "idle");
}

int upd_begin(struct upd *u, const uint8_t *data, size_t n)
{
    if (u->fd >= 0) {
        close(u->fd);
        u->fd = -1;
    }
    if (n != 36) {
        fail(u, EINVAL, "begin: bad request size");
        return -EINVAL;
    }
    uint32_t len = (uint32_t)(data[0] | data[1] << 8 | data[2] << 16 | (uint32_t)data[3] << 24);
    if (len < 64 || len > UPD_MAX_SIZE) {
        fail(u, EINVAL, "begin: bad length");
        return -EINVAL;
    }
    u->fd = open(u->tmp, O_RDWR | O_CREAT | O_TRUNC, 0755);
    if (u->fd < 0) {
        int e = errno;
        fail(u, e, "begin: cannot create the temporary file");
        return -e;
    }
    memcpy(u->want, data + 4, 32);
    u->len = len;
    u->got = 0;
    sha256_init(&u->h);
    u->state = UPD_RECEIVING;
    snprintf(u->last, sizeof u->last, "receiving %u bytes", len);
    LOGI(TAG, "update started: %u bytes", len);
    return 0;
}

int upd_data(struct upd *u, uint32_t offset, const uint8_t *data, size_t n)
{
    if (u->state != UPD_RECEIVING) {
        fail(u, EINVAL, "data: no update in progress");
        return -EINVAL;
    }
    if (offset != u->got || n > UPD_CHUNK || (uint64_t)u->got + n > u->len) {
        fail(u, EINVAL, "data: out-of-order or oversized chunk");
        return -EINVAL;
    }
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(u->fd, data + done, n - done);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            int e = w < 0 ? errno : EIO;
            fail(u, e, "data: write failed (disk full?)");
            return -e;
        }
        done += (size_t)w;
    }
    sha256_update(&u->h, data, n);
    u->got += (uint32_t)n;
    return 0;
}

int upd_commit(struct upd *u)
{
    if (u->state != UPD_RECEIVING) {
        fail(u, EINVAL, "commit: no update in progress");
        return -EINVAL;
    }
    if (u->got != u->len) {
        fail(u, EINVAL, "commit: incomplete upload");
        return -EINVAL;
    }
    uint8_t got[32];
    sha256_final(&u->h, got);
    if (memcmp(got, u->want, 32)) {
        fail(u, EINVAL, "commit: SHA-256 mismatch, upload corrupted");
        return -EBADMSG;
    }
    uint8_t magic[4];
    if (pread(u->fd, magic, 4, 0) != 4 || memcmp(magic, "\x7f" "ELF", 4)) {
        fail(u, EINVAL, "commit: not an ELF executable");
        return -ENOEXEC;
    }
    if (fchmod(u->fd, 0755) || fsync(u->fd)) {
        int e = errno;
        fail(u, e, "commit: cannot flush the new binary");
        return -e;
    }
    close(u->fd);
    u->fd = -1;

    unlink(u->prev);
    if (link(u->target, u->prev) && errno != ENOENT)
        LOGW(TAG, "could not keep the previous binary: %s", strerror(errno));
    if (rename(u->tmp, u->target)) {
        int e = errno;
        unlink(u->tmp);
        u->state = UPD_IDLE;
        snprintf(u->last, sizeof u->last, "error: cannot install the new binary");
        LOGW(TAG, "rename failed: %s", strerror(e));
        return -e;
    }
    int pf = open(u->pending, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (pf >= 0) {
        if (write(pf, "0\n", 2) < 0)
            LOGW(TAG, "could not write the rollback marker");
        fsync(pf);
        close(pf);
    }
    sync();
    u->state = UPD_DONE;
    snprintf(u->last, sizeof u->last, "committed: restarting");
    LOGI(TAG, "update installed (%u bytes), restarting", u->len);
    if (u->restart)
        u->restart();
    return 0;
}

size_t upd_status(const struct upd *u, char *buf, size_t size)
{
    const char *st = u->state == UPD_IDLE ? "idle" : u->state == UPD_RECEIVING ? "receiving" : "done";
    int pending = access(u->pending, F_OK) == 0;
    int n = snprintf(buf, size, "pi-radiogadget %s (%s %s) state=%s got=%u/%u pending=%d last=\"%s\"", PIRG_VERSION,
                     __DATE__, __TIME__, st, u->got, u->len, pending, u->last);
    size_t len = n < 0 ? 0 : ((size_t)n >= size ? size - 1 : (size_t)n);
    if (u->extra && len + 2 < size) {
        buf[len++] = ' ';
        len += u->extra(u->extra_ctx, buf + len, size - len);
    }
    return len;
}

int upd_stop(struct upd *u)
{
    int fd = open(u->disable, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        int e = errno;
        snprintf(u->last, sizeof u->last, "error: cannot create %.60s", u->disable);
        LOGW(TAG, "maintenance mode: cannot create %s: %s", u->disable, strerror(e));
        return -e;
    }
    close(fd);
    sync();
    LOGW(TAG, "maintenance mode requested over USB: %s created, leaving", u->disable);
    snprintf(u->last, sizeof u->last, "stopping");
    if (u->stop)
        u->stop();
    return 0;
}
