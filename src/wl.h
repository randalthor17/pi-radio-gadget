/* Wire formats of the Broadcom FullMAC "dongle" protocol, as the Linux brcmfmac driver
 * speaks it (drivers/net/wireless/broadcom/brcm80211/brcmfmac). Numbers were taken from
 * that driver's headers (fwil_types.h, fweh.h, bcdc.c). All multi-byte fields little endian
 * unless noted (event headers are big endian). Structs are laid out with natural alignment on
 * purpose: that is how the firmware compiles them, and the driver mirrors it.
 */
#pragma once
#include <stdint.h>

/* ---- firmware error codes returned in the dcmd status field */
#define BCME_OK             0
#define BCME_ERROR         (-1)
#define BCME_BADARG        (-2)
#define BCME_NOTASSOCIATED (-17)
#define BCME_UNSUPPORTED   (-23)
#define BCME_BADLEN        (-24)

/* ---- BCDC control message (ep0 payload) */
struct dcmd_hdr {
    uint32_t cmd;
    uint32_t len;         /* buffer length that follows the header */
    uint32_t flags;       /* bit0 error, bit1 set, 15:12 ifidx, 31:16 request id */
    uint32_t status;
};
#define DCMD_ERROR        0x01u
#define DCMD_SET          0x02u
#define DCMD_IF_SHIFT     12
#define DCMD_IF_MASK      0xF000u
#define DCMD_ID_SHIFT     16
#define DCMD_MAXLEN       8192

/* ---- ioctl command numbers (BRCMF_C_*) */
enum {
    WLC_GET_VERSION = 1, WLC_UP = 2, WLC_DOWN = 3, WLC_SET_PROMISC = 10, WLC_GET_RATE = 12,
    WLC_GET_INFRA = 19, WLC_SET_INFRA = 20, WLC_GET_AUTH = 21, WLC_SET_AUTH = 22,
    WLC_GET_BSSID = 23, WLC_GET_SSID = 25, WLC_SET_SSID = 26, WLC_TERMINATED = 28,
    WLC_GET_CHANNEL = 29, WLC_SET_CHANNEL = 30, WLC_GET_SRL = 31, WLC_SET_SRL = 32,
    WLC_GET_LRL = 33, WLC_SET_LRL = 34, WLC_GET_RADIO = 37, WLC_SET_RADIO = 38,
    WLC_GET_PHYTYPE = 39, WLC_SET_KEY = 45, WLC_GET_REGULATORY = 46, WLC_SET_REGULATORY = 47,
    WLC_SET_PASSIVE_SCAN = 49, WLC_SCAN = 50, WLC_SCAN_RESULTS = 51, WLC_DISASSOC = 52,
    WLC_REASSOC = 53, WLC_SET_ROAM_TRIGGER = 55, WLC_SET_ROAM_DELTA = 57,
    WLC_GET_BCNPRD = 75, WLC_SET_BCNPRD = 76, WLC_GET_DTIMPRD = 77, WLC_SET_DTIMPRD = 78,
    WLC_SET_COUNTRY = 84, WLC_GET_PM = 85, WLC_SET_PM = 86, WLC_GET_REVINFO = 98,
    WLC_GET_MONITOR = 107, WLC_SET_MONITOR = 108, WLC_GET_CURR_RATESET = 114,
    WLC_GET_AP = 117, WLC_SET_AP = 118, WLC_SET_SCB_AUTHORIZE = 121, WLC_SET_SCB_DEAUTHORIZE = 122,
    WLC_GET_RSSI = 127, WLC_GET_WSEC = 133, WLC_SET_WSEC = 134, WLC_GET_PHY_NOISE = 135,
    WLC_GET_BSS_INFO = 136, WLC_GET_GET_PKTCNTS = 137, WLC_GET_BANDLIST = 140,
    WLC_SET_SCB_TIMEOUT = 158, WLC_GET_ASSOCLIST = 159, WLC_GET_PHYLIST = 180,
    WLC_SET_SCAN_CHANNEL_TIME = 185, WLC_SET_SCAN_UNASSOC_TIME = 187,
    WLC_SCB_DEAUTHENTICATE_FOR_REASON = 201, WLC_SET_ASSOC_PREFER = 205,
    WLC_GET_VALID_CHANNELS = 217, WLC_SET_FAKEFRAG = 219, WLC_GET_KEY_PRIMARY = 235,
    WLC_SET_KEY_PRIMARY = 236, WLC_SET_SCAN_PASSIVE_TIME = 258,
    WLC_GET_VAR = 262, WLC_SET_VAR = 263, WLC_SET_WSEC_PMK = 268,
};

/* ---- firmware events (BRCMF_E_*) */
enum {
    EVT_SET_SSID = 0, EVT_JOIN = 1, EVT_AUTH = 3, EVT_DEAUTH = 5, EVT_DEAUTH_IND = 6,
    EVT_ASSOC = 7, EVT_ASSOC_IND = 8, EVT_DISASSOC = 11, EVT_DISASSOC_IND = 12, EVT_LINK = 16, EVT_ROAM = 19,
    EVT_PSK_SUP = 46, EVT_IF = 54, EVT_ESCAN_RESULT = 69,
};
#define EVTST_SUCCESS    0
#define EVTST_FAIL       1
#define EVTST_TIMEOUT    2
#define EVTST_NO_NETWORKS 3
#define EVTST_ABORT      4
#define EVTST_PARTIAL    8
#define EVTST_FWSUP_COMPLETED 6
#define EVT_FLAG_LINK    0x01

/* ---- chanspec, D11N encoding (what GET_VERSION == 1 selects), 20 MHz only */
#define CHSPEC_2G_20(ch)   ((uint16_t)(0x2000u | 0x0800u | 0x0300u | ((ch) & 0xffu)))
#define CHSPEC_CH(cs)      ((cs) & 0xffu)

/* ---- scan results */
#define BSS_INFO_VERSION  109
#define BSS_INFO_MAX      2048
#define BSS_MAX_IE        1500

struct bss_info_le {
    uint32_t version;
    uint32_t length;               /* from `version` through the last IE byte */
    uint8_t  BSSID[6];
    uint16_t beacon_period;
    uint16_t capability;
    uint8_t  SSID_len;
    uint8_t  SSID[32];
    struct { uint32_t count; uint8_t rates[16]; } rateset;
    uint16_t chanspec;
    uint16_t atim_window;
    uint8_t  dtim_period;
    int16_t  RSSI;
    int8_t   phy_noise;
    uint8_t  n_cap;
    uint32_t nbss_cap;
    uint8_t  ctl_ch;
    uint32_t reserved32[1];
    uint8_t  flags;
    uint8_t  reserved[3];
    uint8_t  basic_mcs[16];
    uint16_t ie_offset;
    uint32_t ie_length;
    int16_t  SNR;
};
_Static_assert(sizeof(struct bss_info_le) == 128, "bss_info_le must match the firmware layout");

struct escan_result_le {
    uint32_t buflen;
    uint32_t version;
    uint16_t sync_id;
    uint16_t bss_count;
    /* struct bss_info_le follows */
};
_Static_assert(sizeof(struct escan_result_le) == 12, "escan_result header size");

/* ---- misc */
struct ssid_le { uint32_t len; uint8_t ssid[32]; };
struct wsec_pmk_le { uint16_t key_len; uint16_t flags; uint8_t key[128]; };
#define WSEC_PASSPHRASE 0x0001

/* wpa_auth iovar values */
#define WPA_AUTH_DISABLED  0x0000
#define WPA_AUTH_UNSPECIFIED 0x0002
#define WPA_AUTH_PSK       0x0004
#define WPA2_AUTH_UNSPECIFIED 0x0040
#define WPA2_AUTH_PSK      0x0080
#define WPA2_AUTH_1X_SHA256 0x1000
#define WPA2_AUTH_PSK_SHA256 0x8000
#define WPA3_AUTH_SAE_PSK  0x40000
