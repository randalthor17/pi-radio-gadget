/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * pi-radio-gadget-flash: push a new pi-radio-gadget daemon binary to the Pi over the USB link it already provides.
 *
 *   pi-radio-gadget-flash --status
 *   pi-radio-gadget-flash --stop        (maintenance mode: stop the service for good, get SSH over WiFi back)
 *   pi-radio-gadget-flash [--force] path/to/pi-radio-gadget      (a static aarch64 build)
 *
 * Talks to the device (USB 0a5c:0bdc) with vendor control requests through usbfs, no libusb needed. Needs access to
 * /dev/bus/usb/BBB/DDD: run it as root, or install dist/70-pi-radio-gadget.rules.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "sha256.h"
#include "update.h"

static int read_int(const char *path)
{
    FILE *f = fopen(path, "r");
    int v = -1;
    if (f) {
        if (fscanf(f, "%d", &v) != 1)
            v = -1;
        fclose(f);
    }
    return v;
}

static int read_hex(const char *path)
{
    FILE *f = fopen(path, "r");
    int v = -1;
    if (f) {
        unsigned int u;
        v = fscanf(f, "%x", &u) == 1 ? (int)u : -1;
        fclose(f);
    }
    return v;
}

/* Opens the gadget's usbfs node, or returns -1. */
static int open_dev(void)
{
    DIR *d = opendir("/sys/bus/usb/devices");
    if (!d)
        return -1;
    struct dirent *e;
    int fd = -1;
    while (fd < 0 && (e = readdir(d))) {
        char p[512];
        if (e->d_name[0] == '.' || strchr(e->d_name, ':'))
            continue;
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/idVendor", e->d_name);
        int vid = read_hex(p);
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/idProduct", e->d_name);
        int pid = read_hex(p);
        if (vid != 0x0a5c || pid != 0x0bdc)
            continue;
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/busnum", e->d_name);
        int bus = read_int(p);
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/devnum", e->d_name);
        int dev = read_int(p);
        snprintf(p, sizeof p, "/dev/bus/usb/%03d/%03d", bus, dev);
        fd = open(p, O_RDWR);
        if (fd < 0 && errno == EACCES)
            fprintf(stderr, "cannot open %s: %s (run as root, or install the udev rule)\n", p, strerror(errno));
    }
    closedir(d);
    return fd;
}

static int ctrl(int fd, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void *data, uint16_t len)
{
    struct usbdevfs_ctrltransfer t = { .bRequestType = type, .bRequest = req, .wValue = val, .wIndex = idx,
                                       .wLength = len, .timeout = 5000, .data = data };
    return ioctl(fd, USBDEVFS_CONTROL, &t);
}

static int status(int fd, char *out, size_t size)
{
    int n = ctrl(fd, 0xC0, UPD_REQ_STATUS, 0, 0, out, (uint16_t)(size - 1));
    if (n < 0)
        return -1;
    out[n] = 0;
    return n;
}

static int wait_device(int secs)
{
    for (int i = 0; i < secs * 4; i++) {
        int fd = open_dev();
        if (fd >= 0)
            return fd;
        struct timespec ts = { 0, 250000000 };
        nanosleep(&ts, NULL);
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *file = NULL;
    int want_status = 0, force = 0, want_stop = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--status")) want_status = 1;
        else if (!strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "--stop")) want_stop = 1;
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
        else file = argv[i];
    }
    if (!want_status && !want_stop && !file) {
        fprintf(stderr, "usage: %s --status | --stop | [--force] BINARY\n", argv[0]);
        return 2;
    }
    int fd = open_dev();
    if (fd < 0) {
        fprintf(stderr, "Pi gadget (0a5c:0bdc) not found\n");
        return 1;
    }
    char st[1024];
    if (status(fd, st, sizeof st) < 0) {
        fprintf(stderr, "the device does not answer the update protocol (old daemon version: update it once via the SD card)\n");
        return 1;
    }
    printf("running: %s\n", st);
    if (want_status)
        return 0;
    if (want_stop) {
        char stop[4] = { 'S', 'T', 'O', 'P' };
        if (ctrl(fd, 0x40, UPD_REQ_STOP, 0, 0, stop, sizeof stop) < 0 && errno != ENODEV && errno != EPROTO) {
            perror("stop");
            return 1;
        }
        printf("maintenance mode: the service is stopping and stays disabled (the Pi gets its own WiFi and SSH back).\n"
               "To end it: ssh to the Pi, then  sudo rm /boot/firmware/pi-radio-gadget.disable && sudo systemctl start pi-radio-gadget\n");
        return 0;
    }

    FILE *f = fopen(file, "rb");
    if (!f) { perror(file); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 64 || (unsigned long)size > UPD_MAX_SIZE) { fprintf(stderr, "bad file size\n"); return 1; }
    uint8_t *img = malloc((size_t)size);
    if (!img || fread(img, 1, (size_t)size, f) != (size_t)size) { fprintf(stderr, "read failed\n"); return 1; }
    fclose(f);
    if (memcmp(img, "\x7f" "ELF", 4) || (!force && (img[4] != 2 || img[18] != 0xB7 || img[19] != 0))) {
        fprintf(stderr, "%s is not a 64-bit ARM (aarch64) ELF executable; use --force to send it anyway\n", file);
        return 1;
    }
    struct sha256 h;
    uint8_t req[36];
    sha256_init(&h);
    sha256_update(&h, img, (size_t)size);
    uint8_t hash[32];
    sha256_final(&h, hash);
    for (int i = 0; i < 4; i++) req[i] = (uint8_t)((uint32_t)size >> (8 * i));
    memcpy(req + 4, hash, 32);

    printf("sending %s (%ld bytes)\n", file, size);
    if (ctrl(fd, 0x40, UPD_REQ_BEGIN, 0, 0, req, sizeof req) < 0) { perror("begin"); return 1; }
    for (long o = 0; o < size; o += UPD_CHUNK) {
        uint16_t n = (uint16_t)(size - o < UPD_CHUNK ? size - o : UPD_CHUNK);
        if (ctrl(fd, 0x40, UPD_REQ_DATA, (uint16_t)(o & 0xffff), (uint16_t)((unsigned long)o >> 16), img + o, n) < 0) {
            fprintf(stderr, "\ntransfer failed at byte %ld: %s\n", o, strerror(errno));
            if (status(fd, st, sizeof st) > 0) fprintf(stderr, "device: %s\n", st);
            return 1;
        }
        printf("\r%ld%%", (o + n) * 100 / size);
        fflush(stdout);
    }
    printf("\n");
    if (status(fd, st, sizeof st) > 0 && !strstr(st, "state=receiving")) {
        fprintf(stderr, "device refused the upload: %s\n", st);
        return 1;
    }
    char ok[4] = { 'C', 'O', 'M', 'T' };
    if (ctrl(fd, 0x40, UPD_REQ_COMMIT, 0, 0, ok, sizeof ok) < 0 && errno != ENODEV && errno != EPROTO) {
        perror("commit");
        return 1;
    }
    /* A good commit restarts the service: the USB device disappears for a few seconds. */
    struct timespec ts = { 0, 300000000 };
    nanosleep(&ts, NULL);
    close(fd);
    printf("waiting for the device to restart...\n");
    int gone = 0;
    for (int i = 0; i < 40; i++) {       /* up to ~10 s for it to go away */
        int t = open_dev();
        if (t < 0) { gone = 1; break; }
        if (status(t, st, sizeof st) > 0 && strstr(st, "state=idle") && strstr(st, "pending=1")) { close(t); gone = 1; break; }
        if (strstr(st, "error")) { fprintf(stderr, "update failed: %s\n", st); close(t); return 1; }
        close(t);
        nanosleep(&ts, NULL);
    }
    (void)gone;
    fd = wait_device(60);
    if (fd < 0) { fprintf(stderr, "the device did not come back within 60 s\n"); return 1; }
    for (int i = 0; i < 20 && status(fd, st, sizeof st) < 0; i++)
        nanosleep(&ts, NULL);
    printf("now running: %s\n", st);
    if (strstr(st, "pending=1"))
        printf("OK: new version is up; it is confirmed after 60 s of uptime (otherwise the Pi rolls back).\n");
    return 0;
}
