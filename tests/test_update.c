/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "sha256.h"
#include "update.h"

static int g_restarts;
static void on_restart(void) { g_restarts++; }
static int g_stops;
static void on_stop(void) { g_stops++; }

static void hexof(const uint8_t *h, char *out)
{
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", h[i]);
}

static void begin_req(uint8_t *req, uint32_t len, const uint8_t *hash)
{
    req[0] = (uint8_t)len; req[1] = (uint8_t)(len >> 8); req[2] = (uint8_t)(len >> 16); req[3] = (uint8_t)(len >> 24);
    memcpy(req + 4, hash, 32);
}

static int upload(struct upd *u, const uint8_t *img, uint32_t len, const uint8_t *hash)
{
    uint8_t req[36];
    begin_req(req, len, hash);
    int r = upd_begin(u, req, sizeof req);
    for (uint32_t o = 0; !r && o < len; o += UPD_CHUNK)
        r = upd_data(u, o, img + o, len - o < UPD_CHUNK ? len - o : UPD_CHUNK);
    return r ? r : upd_commit(u);
}

int main(void)
{
    uint8_t h[32];
    char hex[65];
    struct sha256 s;
    sha256_init(&s); sha256_final(&s, h); hexof(h, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256 of empty: %s", hex);
    sha256_init(&s); sha256_update(&s, "abc", 3); sha256_final(&s, h); hexof(h, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256 of abc: %s", hex);
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";    /* 56 bytes: padding spills a block */
    sha256_init(&s); sha256_update(&s, m, 56); sha256_final(&s, h); hexof(h, hex);
    CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256 of 56 bytes: %s", hex);
    uint8_t a[1000];
    memset(a, 'a', sizeof a);
    sha256_init(&s);
    for (int i = 0; i < 1000; i++) sha256_update(&s, a, 1000);
    sha256_final(&s, h); hexof(h, hex);
    CHECK(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "sha256 of a million a: %s", hex);

    char dir[] = "/tmp/pirg-upd-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp");
    char target[300];
    snprintf(target, sizeof target, "%s/pi-radio-gadget", dir);
    FILE *f = fopen(target, "w"); fputs("OLD-BINARY", f); fclose(f);

    static uint8_t img[10000];
    memcpy(img, "\x7f" "ELF", 4);
    for (size_t i = 4; i < sizeof img; i++) img[i] = (uint8_t)(i * 7);
    sha256_init(&s); sha256_update(&s, img, sizeof img); sha256_final(&s, h);

    struct upd u;
    upd_init(&u, target, on_restart);
    char st[300];
    upd_status(&u, st, sizeof st);
    CHECK(strstr(st, "state=idle") && strstr(st, "pending=0"), "initial status: %s", st);

    /* a corrupted upload is refused and leaves the running binary alone */
    uint8_t bad[32]; memcpy(bad, h, 32); bad[5] ^= 1;
    CHECK(upload(&u, img, sizeof img, bad) != 0, "wrong hash is refused");
    CHECK(g_restarts == 0, "no restart after a refused update");
    char buf[32] = { 0 };
    int fd = open(target, O_RDONLY); CHECK(read(fd, buf, 10) == 10, "read target"); close(fd);
    CHECK(!strcmp(buf, "OLD-BINARY"), "target untouched after a refused update");
    char tmp[310]; snprintf(tmp, sizeof tmp, "%s.new", target);
    CHECK(access(tmp, F_OK) != 0, "temporary file removed after a refused update");

    /* out-of-order chunk */
    uint8_t req[36]; begin_req(req, sizeof img, h);
    CHECK(upd_begin(&u, req, sizeof req) == 0, "begin");
    CHECK(upd_data(&u, 4096, img + 4096, 100) != 0, "out-of-order chunk refused");

    /* a non-ELF file with a correct hash is refused */
    static uint8_t txt[200]; memset(txt, 'x', sizeof txt);
    sha256_init(&s); sha256_update(&s, txt, sizeof txt); uint8_t th[32]; sha256_final(&s, th);
    CHECK(upload(&u, txt, sizeof txt, th) != 0, "non-ELF refused");

    /* the good one */
    CHECK(upload(&u, img, sizeof img, h) == 0, "good update accepted");
    CHECK(g_restarts == 1, "restart requested once");
    static uint8_t back[sizeof img];
    fd = open(target, O_RDONLY); ssize_t n = read(fd, back, sizeof back); close(fd);
    CHECK(n == (ssize_t)sizeof img && !memcmp(back, img, sizeof img), "target now holds the new image");
    struct stat sb; stat(target, &sb);
    CHECK((sb.st_mode & 0111) == 0111, "target is executable");
    char prev[310]; snprintf(prev, sizeof prev, "%s.prev", target);
    memset(buf, 0, sizeof buf);
    fd = open(prev, O_RDONLY); CHECK(read(fd, buf, 10) == 10, "read prev"); close(fd);
    CHECK(!strcmp(buf, "OLD-BINARY"), "previous binary kept as .prev");
    char pend[310]; snprintf(pend, sizeof pend, "%s/pending", dir);
    CHECK(access(pend, F_OK) == 0, "rollback marker written");
    upd_status(&u, st, sizeof st);
    CHECK(strstr(st, "state=done") && strstr(st, "pending=1"), "final status: %s", st);

    /* Maintenance mode: the disable file appears and the daemon is asked to stop. */
    char dis[272]; snprintf(dis, sizeof dis, "%.250s/disable", dir);
    snprintf(u.disable, sizeof u.disable, "%s", dis);
    u.stop = on_stop;
    CHECK(upd_stop(&u) == 0 && access(dis, F_OK) == 0 && g_stops == 1, "stop creates the disable file and stops the daemon");
    snprintf(u.disable, sizeof u.disable, "%s", "/nonexistent-dir/disable");
    CHECK(upd_stop(&u) < 0 && g_stops == 1, "stop refused when the file cannot be created");
    unlink(dis);

    unlink(target); unlink(prev); unlink(pend); rmdir(dir);
    TEST_DONE("update");
}
