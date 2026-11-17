/* Ethernet tap on a network interface (AF_PACKET): the data path of the WiFi radio backend.
 *
 * The Pi's FullMAC WiFi chip converts between 802.11 and Ethernet itself, so the data path is
 * simply "Ethernet frames in, Ethernet frames out" on the interface:
 *   tap_send()  puts a frame from the PC on the wire,
 *   the rx callback delivers frames that arrived for us.
 *
 * Rules enforced here (each one is a real failure mode):
 *   - frames the Pi itself transmitted are not looped back (PACKET_OUTGOING)
 *   - EAPOL (0x888e) is neither forwarded nor delivered: the WPA handshake belongs to the Pi's chip
 *   - a FullMAC station can only transmit as itself, so a foreign source MAC is rewritten
 */
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*tap_rx_fn)(const uint8_t *eth, size_t len, void *ctx);

struct tap {
    int      fd;
    int      ifindex;
    uint8_t  mac[6];                  /* the station address frames are sent from */
    tap_rx_fn rx;
    void    *ctx;
    atomic_bool enabled;              /* frames are only passed while enabled */
    pthread_t th;
    atomic_ulong rx_frames, tx_frames, sa_rewrites, eapol_dropped, tx_dropped;
};

int  tap_open(struct tap *t, const char *ifname);
int  tap_start(struct tap *t, tap_rx_fn rx, void *ctx);   /* starts the receive thread */
void tap_close(struct tap *t);
/* Returns true if the frame was handed to the interface. */
bool tap_send(struct tap *t, const uint8_t *eth, size_t len);
