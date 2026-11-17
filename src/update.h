#pragma once
/*
 * Updating the daemon from the PC over the USB link, no network and no SD card.
 *
 * Protocol: vendor control requests to the device (bmRequestType 0x40 OUT / 0xC0 IN, recipient device):
 *   UPD_REQ_BEGIN  OUT  data = u32 LE length, 32-byte SHA-256 of the new binary
 *   UPD_REQ_DATA   OUT  data = next chunk (<= UPD_CHUNK); wValue = offset bits 0..15, wIndex = offset bits 16..31
 *   UPD_REQ_COMMIT OUT  data = "COMT" (a data stage keeps the request simple for FunctionFS)
 *   UPD_REQ_STATUS IN   returns a text line: version, state, result of the last step
 *
 * Commit verifies length, hash and ELF magic, keeps the running binary as <target>.prev, replaces the target
 * atomically, writes <dir>/pending and asks the daemon to restart. The service script rolls back to .prev if the new
 * binary does not stay up (see dist/pi-radiogadget-update.sh); the daemon removes pending once it has run 60 s.
 */
#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

#define UPD_REQ_BEGIN  0x50
#define UPD_REQ_DATA   0x51
#define UPD_REQ_COMMIT 0x52
#define UPD_REQ_STATUS 0x53
#define UPD_REQ_STOP   0x54   // OUT, data "STOP": maintenance mode, see upd_stop()
#define UPD_CHUNK      4096
#define UPD_MAX_SIZE   (16u * 1024 * 1024)

#ifndef PIRG_VERSION
#define PIRG_VERSION "0.4.0"
#endif

enum upd_state { UPD_IDLE, UPD_RECEIVING, UPD_DONE };

struct upd {
    char     target[256];
    char     tmp[272];
    char     prev[272];
    char     pending[272];
    int      fd;
    int      state;
    uint32_t len, got;
    uint8_t  want[32];
    struct sha256 h;
    char     last[96];
    void   (*restart)(void);
    void   (*stop)(void);                                  /* leave for good: the service must not start again */
    char     disable[272];                                 /* the file whose presence keeps the service from starting */
    size_t (*extra)(void *ctx, char *buf, size_t size);   /* appended to the status line (live counters) */
    void    *extra_ctx;
};

/* target = the running binary's path. restart is called after a successful commit (may be NULL). */
void upd_init(struct upd *u, const char *target, void (*restart)(void));
int  upd_begin(struct upd *u, const uint8_t *data, size_t n);
int  upd_data(struct upd *u, uint32_t offset, const uint8_t *data, size_t n);
int  upd_commit(struct upd *u);
/* Maintenance mode: creates the disable file (so the service stays off, also after a reboot) and asks the daemon to exit.
 * The Pi then gets its own WiFi back, with SSH. Remove the file and start the service to leave maintenance mode. */
int  upd_stop(struct upd *u);
/* Writes the status line (no newline needed by the caller); returns its length. */
size_t upd_status(const struct upd *u, char *buf, size_t size);
