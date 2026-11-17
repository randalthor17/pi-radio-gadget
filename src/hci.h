/* Bluetooth function: the PC's stack sees a USB Bluetooth dongle (class E0/01/01);
 * every byte is relayed to the Pi's real controller through an HCI *user channel* socket.
 *
 *   PC host <-USB-> dwc2 UDC -> FunctionFS -> [this] -> HCI user channel -> BT chip
 *
 * Nothing about Bluetooth contents is interpreted; this is framing and plumbing.
 *
 * USB side (Bluetooth USB transport):
 *   ep0 control   HCI commands   (class request, bRequest 0, data stage = the command)
 *   int IN        HCI events
 *   bulk IN       ACL data to the host
 *   bulk OUT      ACL data from the host
 * Call audio (SCO over isochronous endpoints) is a separate USB function, ffs.sco, driven from this file too: see sco.h and
 * docs/ARCHITECTURE.md §4.
 */
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "acl_split.h"
#include "ffs.h"
#include "pktq.h"
#include "sco.h"

#define H4_CMD 1
#define H4_ACL 2
#define H4_SCO 3
#define H4_EVT 4

#define HCI_EP_EVT_IN  0x81
#define HCI_EP_ACL_IN  0x82
#define HCI_EP_ACL_OUT 0x03
/* SCO audio: interface 1, alternate settings 1 and 2, each with its own isochronous endpoints (see sco.h). */
#define HCI_EP_SCO1_OUT 0x04
#define HCI_EP_SCO1_IN  0x85
#define HCI_EP_SCO2_OUT 0x06
#define HCI_EP_SCO2_IN  0x87

struct upd;

struct hci_fn {
    /* configuration */
    char     ffs_dir[128];
    char     ffs_sco_dir[128];          /* the audio function (see hci_fill_sco_spec) */
    int      hci_dev;
    unsigned int_maxp;
    bool     zlp;
    struct upd *upd;             /* optional: handles the over-USB update requests */
    bool     sco_over_hci;       /* route call audio over HCI (vendor command after each reset). Experimental: see README */
    /* runtime */
    int      ep0_sco;
    int      sock, ep0, ep_evt, ep_acl_in, ep_acl_out;
    int      ep_sco_out[2], ep_sco_in[2];            /* [0] = alternate setting 1, [1] = alternate setting 2 */
    struct pktq q_sco[2];
    struct ep_writer w_sco[2];
    struct sco_reasm reasm[2];
    atomic_int  sco_alt;                           /* 1 or 2: which alternate setting the host is using */
    atomic_bool swallow_vendor;                    /* the answer to our own vendor command must not reach the host */
    unsigned long sco_to_pc, sco_to_chip;
    unsigned long sco_err[2];                       /* failed audio writes to the host, per alternate setting */
    unsigned long sco_rd[2], sco_rd_err[2];         /* reads from the host's audio OUT endpoints: frames / failures */
    int           sco_rd_errno[2];
    int           sco_err_last[2];                  /* errno of the last one */
    pthread_mutex_t send_mu;
    struct pktq q_evt, q_acl;
    struct ep_writer w_evt, w_acl;
    struct acl_split split;
    atomic_bool  enabled;
    atomic_uint  epoch;               /* bumped on every (re)enable so stale partial data is discarded */
    unsigned long to_chip, from_chip, sco_dropped;
};

void hci_fill_spec(struct ffs_fn_spec *spec, unsigned int_maxp);
/* The audio interface (alternate settings 0..2), a function of its own. */
void hci_fill_sco_spec(struct ffs_fn_spec *spec);
/* Frees the controller from the kernel stack and binds the user channel. Returns fd or -errno. */
int  hci_open_user_channel(int dev);

/* Phase 1: bind the controller, write descriptors. Call before the UDC is attached. */
int  hci_fn_prepare(struct hci_fn *h);
/* Phase 2: after the UDC is attached: open endpoints and start the worker threads. */
int  hci_fn_run(struct hci_fn *h);
/* One line of live counters (call audio, queues) for the status request. Returns the length written. */
size_t hci_stats_line(void *h, char *buf, size_t size);
