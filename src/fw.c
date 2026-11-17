/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "fw.h"

#include <endian.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdc.h"
#include "log.h"

#define TAG "fw"

/* ---------------------------------------------------------------- small helpers */

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return le32toh(v); }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return le16toh(v); }
static void wr32(uint8_t *p, uint32_t v) { v = htole32(v); memcpy(p, &v, 4); }

struct out {                          /* response data area */
    uint8_t *p;
    size_t   cap;
    size_t   len;
};

static int out_put(struct out *o, const void *d, size_t n)
{
    if (o->len + n > o->cap)
        return BCME_BADLEN;
    memcpy(o->p + o->len, d, n);
    o->len += n;
    return BCME_OK;
}

static int out_u32(struct out *o, uint32_t v)
{
    uint8_t b[4];
    wr32(b, v);
    return out_put(o, b, 4);
}

static int out_str(struct out *o, const char *s)
{
    return out_put(o, s, strlen(s) + 1);
}

/* ---------------------------------------------------------------- iovar store */

static struct fw_kv *kv_find(struct fw *f, const char *name)
{
    for (int i = 0; i < f->n_kv; i++)
        if (strcmp(f->kv[i].name, name) == 0)
            return &f->kv[i];
    return NULL;
}

static void kv_set(struct fw *f, const char *name, const void *val, size_t len)
{
    struct fw_kv *k = kv_find(f, name);
    if (!k) {
        if (f->n_kv >= FW_MAX_KV || strlen(name) >= sizeof k->name)
            return;
        k = &f->kv[f->n_kv++];
        snprintf(k->name, sizeof k->name, "%s", name);
    }
    if (len > sizeof k->val)
        len = sizeof k->val;
    memcpy(k->val, val, len);
    k->len = len;
}

static void kv_set_u32(struct fw *f, const char *name, uint32_t v)
{
    uint8_t b[4];
    wr32(b, v);
    kv_set(f, name, b, 4);
}

/* ---------------------------------------------------------------- events */

static bool evt_wanted(const struct fw *f, uint32_t code)
{
    if (code == EVT_IF)
        return true;
    if (code / 8 >= sizeof f->event_mask)
        return false;
    return f->event_mask[code / 8] & (1u << (code % 8));
}

static void emit(struct fw *f, uint32_t code, uint32_t status, uint32_t reason, uint16_t flags,
                 const uint8_t *addr, const void *data, size_t dlen)
{
    if (!f->host_enabled || !evt_wanted(f, code)) {
        LOGD(TAG, "event %u not delivered (%s)", code, f->host_enabled ? "masked" : "host not enabled");
        return;
    }
    uint8_t buf[BDC_HDR_LEN + ETH_HLEN_ + 10 + 48 + 4096];
    size_t n = bdc_build_event(buf, sizeof buf, f->mac, code, status, reason, flags, addr, 0, data, dlen);
    if (!n) {
        LOGW(TAG, "event %u too large (%zu)", code, dlen);
        return;
    }
    LOGD(TAG, "event %u status=%u reason=%u flags=%#x datalen=%zu", code, status, reason, flags, dlen);
    f->to_host(buf, n, f->to_host_ctx);
}

/* ---------------------------------------------------------------- BSS info */

static bool ie_has(const uint8_t *ie, size_t len, uint8_t id)
{
    while (len >= 2) {
        size_t l = ie[1];
        if (l + 2 > len)
            break;
        if (ie[0] == id)
            return true;
        ie += l + 2;
        len -= l + 2;
    }
    return false;
}

size_t fw_build_bss_info(uint8_t *out, size_t cap, const struct wl_bss *b)
{
    size_t total = sizeof(struct bss_info_le) + b->ie_len;
    if (cap < total)
        return 0;
    struct bss_info_le bi;
    memset(&bi, 0, sizeof bi);
    bi.version = htole32(BSS_INFO_VERSION);
    bi.length = htole32((uint32_t)total);
    memcpy(bi.BSSID, b->bssid, 6);
    bi.beacon_period = htole16(b->beacon_int ? b->beacon_int : 100);
    bi.capability = htole16(b->capability);
    bi.SSID_len = b->ssid_len;
    memcpy(bi.SSID, b->ssid, b->ssid_len);
    static const uint8_t rates[8] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
    bi.rateset.count = htole32(8);
    memcpy(bi.rateset.rates, rates, 8);
    bi.chanspec = htole16(CHSPEC_2G_20(b->channel));
    bi.dtim_period = 1;
    bi.RSSI = (int16_t)htole16((uint16_t)(int16_t)b->rssi);
    bi.phy_noise = -90;
    bi.n_cap = ie_has(b->ie, b->ie_len, 45) ? 1 : 0;      /* HT capabilities IE */
    bi.ctl_ch = b->channel;
    bi.ie_offset = htole16(sizeof bi);
    bi.ie_length = htole32(b->ie_len);
    bi.SNR = htole16((uint16_t)(b->rssi + 90));
    memcpy(out, &bi, sizeof bi);
    memcpy(out + sizeof bi, b->ie, b->ie_len);
    return total;
}

/* The host driver posts 1600-byte receive buffers. A larger frame makes the USB endpoint babble and the
 * driver never resubmits that buffer, so every frame we send must fit. */
#define HOST_RX_MAX      1600
#define EVT_OVERHEAD     (BDC_HDR_LEN + ETH_HLEN_ + 10 + 48)
#define ESCAN_FIXED      ((int)sizeof(struct escan_result_le) + (int)sizeof(struct bss_info_le))
#define MAX_EVENT_IE     (HOST_RX_MAX - EVT_OVERHEAD - ESCAN_FIXED)

/* Largest prefix of the IE blob, at most max bytes, that ends on an element boundary. */
size_t fw_trim_ies(const uint8_t *ie, size_t len, size_t max)
{
    size_t o = 0;
    while (o + 2 <= len && o + 2 + ie[o + 1] <= len && o + 2 + ie[o + 1] <= max)
        o += 2u + ie[o + 1];
    return o;
}

/* ---------------------------------------------------------------- scan */

static void parse_escan(struct fw *f, const uint8_t *d, size_t n)
{
    /* version u32, action u16, sync_id u16, then brcmf_scan_params_le (v1):
     *   ssid_le(36) bssid(6) bss_type(1) scan_type(1) nprobes active passive home (4 each)
     *   channel_num(4) channel_list[] (u16) ; ssids follow, 4-byte aligned */
    if (n < 8)
        return;
    uint16_t action = rd16(d + 4);
    uint16_t sync = rd16(d + 6);

    if (action == 3) {                                     /* abort */
        LOGI(TAG, "escan abort");
        bool was = f->scan_active;
        f->scan_active = false;
        if (f->be->ops->scan_abort)
            f->be->ops->scan_abort(f->be);
        if (was) {
            struct escan_result_le hdr = { .version = htole32(BSS_INFO_VERSION) };
            emit(f, EVT_ESCAN_RESULT, EVTST_ABORT, 0, 0, NULL, &hdr, sizeof hdr);
        }
        return;
    }
    if (action != 1 && action != 2)
        return;

    struct wl_scan_req req;
    memset(&req, 0, sizeof req);
    const uint8_t *p = d + 8;
    size_t left = n - 8;
    if (left < 64) {
        LOGW(TAG, "short escan params (%zu)", left);
        return;
    }
    uint32_t chan_num = rd32(p + 60);
    unsigned nch = chan_num & 0xffff, nssid = chan_num >> 16;
    req.passive = p[45] == 1;
    if (nch > WL_MAX_SCAN_CHANS)
        nch = WL_MAX_SCAN_CHANS;
    size_t off = 64;
    for (unsigned i = 0; i < nch && off + 2 <= left; i++, off += 2) {
        uint16_t cs = rd16(p + off);
        if (cs != 0xffff && CHSPEC_CH(cs) >= 1 && CHSPEC_CH(cs) <= 14)
            req.channels[req.n_channels++] = (uint8_t)CHSPEC_CH(cs);
    }
    off = (64 + 2 * nch + 3) & ~3u;
    if (nssid == 0) {
        uint32_t l = rd32(p);
        if (l > 0 && l <= 32) {
            req.ssids[0].len = (uint8_t)l;
            memcpy(req.ssids[0].ssid, p + 4, l);
            req.n_ssids = 1;
        }
    }
    for (unsigned i = 0; i < nssid && i < WL_MAX_SCAN_SSIDS && off + 36 <= left; i++, off += 36) {
        uint32_t l = rd32(p + off);
        if (l > 32)
            l = 32;
        req.ssids[req.n_ssids].len = (uint8_t)l;
        memcpy(req.ssids[req.n_ssids].ssid, p + off + 4, l);
        req.n_ssids++;
    }

    LOGI(TAG, "escan start sync=%u channels=%u ssids=%u%s", sync, req.n_channels, req.n_ssids,
         req.passive ? " passive" : "");
    f->scan_active = true;
    f->scan_sync_id = sync;
    f->scan_results = 0;
    f->be->ops->scan(f->be, &req);
}

void fw_scan_result(struct fw *f, const struct wl_bss *in)
{
    struct wl_bss trimmed;
    const struct wl_bss *bss = in;
    if (in->ie_len > MAX_EVENT_IE) {
        trimmed = *in;
        trimmed.ie_len = (uint16_t)fw_trim_ies(in->ie, in->ie_len, MAX_EVENT_IE);
        bss = &trimmed;
    }
    pthread_mutex_lock(&f->mu);
    if (f->scan_active) {
        uint8_t buf[sizeof(struct escan_result_le) + BSS_INFO_MAX + 256];
        size_t bn = fw_build_bss_info(buf + sizeof(struct escan_result_le),
                                      sizeof buf - sizeof(struct escan_result_le), bss);
        if (bn && bn <= BSS_INFO_MAX) {
            struct escan_result_le h = {
                .buflen = htole32((uint32_t)(sizeof h + bn)),
                .version = htole32(BSS_INFO_VERSION),
                .sync_id = htole16(f->scan_sync_id),
                .bss_count = htole16(1),
            };
            memcpy(buf, &h, sizeof h);
            emit(f, EVT_ESCAN_RESULT, EVTST_PARTIAL, 0, 0, NULL, buf, sizeof h + bn);
            f->scan_results++;
        }
    }
    pthread_mutex_unlock(&f->mu);
}

void fw_scan_done(struct fw *f, bool aborted)
{
    pthread_mutex_lock(&f->mu);
    if (f->scan_active) {
        f->scan_active = false;
        LOGI(TAG, "escan done: %u results%s", f->scan_results, aborted ? " (aborted)" : "");
        struct escan_result_le h = { .version = htole32(BSS_INFO_VERSION),
                                     .sync_id = htole16(f->scan_sync_id) };
        emit(f, EVT_ESCAN_RESULT, aborted ? EVTST_ABORT : EVTST_SUCCESS, 0, 0, NULL, &h, sizeof h);
    }
    pthread_mutex_unlock(&f->mu);
}

/* ---------------------------------------------------------------- association */

/* Give the radio back: leave the network or stop the hotspot, whichever is running. */
static void radio_release(struct fw *f)
{
    if (f->state == FW_AP) {
        f->n_sta = 0;
        if (f->be->ops->ap_stop)
            f->be->ops->ap_stop(f->be);
    } else
        f->be->ops->leave(f->be);
}

static void start_join(struct fw *f, const uint8_t *ssid, uint32_t ssid_len, const uint8_t *bssid,
                       uint8_t channel)
{
    struct wl_join_req j;
    memset(&j, 0, sizeof j);
    if (ssid_len > 32)
        ssid_len = 32;
    j.ssid_len = (uint8_t)ssid_len;
    memcpy(j.ssid, ssid, ssid_len);
    static const uint8_t bcast[6] = { 255, 255, 255, 255, 255, 255 };
    j.bssid_any = !bssid || memcmp(bssid, bcast, 6) == 0 || memcmp(bssid, "\0\0\0\0\0\0", 6) == 0;
    if (!j.bssid_any)
        memcpy(j.bssid, bssid, 6);
    j.channel = channel;
    j.wpa_auth = f->wpa_auth;
    j.wsec = f->wsec;
    j.mfp = f->mfp;
    j.have_pmk = f->have_pmk;
    memcpy(j.pmk, f->pmk, 32);
    j.have_passphrase = f->have_pass;
    memcpy(j.passphrase, f->pass, sizeof j.passphrase);
    j.have_sae = f->have_sae;
    memcpy(j.sae_password, f->sae_pw, sizeof j.sae_password);

    LOGI(TAG, "join \"%.*s\"%s wpa_auth=%#x wsec=%#x mfp=%u pmk=%d sae=%d ch=%u", j.ssid_len, j.ssid,
         j.bssid_any ? "" : " (bssid pinned)", j.wpa_auth, j.wsec, j.mfp, j.have_pmk, j.have_sae, j.channel);
    f->state = FW_JOINING;
    f->have_bss = false;
    f->be->ops->join(f->be, &j);
}

void fw_join_done(struct fw *f, enum wl_join_result r, const struct wl_bss *bss)
{
    pthread_mutex_lock(&f->mu);
    if (f->state != FW_JOINING) {
        LOGD(TAG, "join result %d ignored (not joining)", r);
        pthread_mutex_unlock(&f->mu);
        return;
    }
    static const uint8_t zero[6];
    const uint8_t *addr = bss ? bss->bssid : zero;

    if (r == WL_JOIN_OK && bss) {
        f->cur_bss = *bss;
        f->have_bss = true;
        f->state = FW_UP;
        LOGI(TAG, "joined \"%.*s\" on channel %u", bss->ssid_len, bss->ssid, bss->channel);
        /* With PSK/SAE offload the firmware reports the handshake finishing, then the link. */
        if (f->sup_wpa)
            emit(f, EVT_PSK_SUP, EVTST_FWSUP_COMPLETED, 0, 0, addr, NULL, 0);
        emit(f, EVT_SET_SSID, EVTST_SUCCESS, 0, 0, addr, NULL, 0);
        emit(f, EVT_LINK, EVTST_SUCCESS, 0, EVT_FLAG_LINK, addr, NULL, 0);
    } else if (r == WL_JOIN_AUTH_FAILED) {
        f->state = FW_IDLE;
        LOGW(TAG, "join failed: authentication/handshake");
        if (f->sup_wpa)
            emit(f, EVT_PSK_SUP, 7 /* FWSUP_TIMEOUT */, 15 /* WPA_PSK_TMO */, 0, addr, NULL, 0);
        emit(f, EVT_SET_SSID, EVTST_FAIL, 0, 0, addr, NULL, 0);
    } else {
        f->state = FW_IDLE;
        LOGW(TAG, "join failed: no such network");
        emit(f, EVT_SET_SSID, EVTST_NO_NETWORKS, 0, 0, addr, NULL, 0);
    }
    pthread_mutex_unlock(&f->mu);
}

void fw_link_lost(struct fw *f, uint16_t reason)
{
    pthread_mutex_lock(&f->mu);
    if (f->state == FW_UP) {
        LOGI(TAG, "link lost (reason %u)", reason);
        f->state = FW_IDLE;
        emit(f, EVT_DEAUTH_IND, EVTST_SUCCESS, reason, 0, f->cur_bss.bssid, NULL, 0);
        emit(f, EVT_LINK, EVTST_SUCCESS, 0, 0, f->cur_bss.bssid, NULL, 0);
    }
    pthread_mutex_unlock(&f->mu);
}

void fw_rx_frame(struct fw *f, const uint8_t *eth, size_t len)
{
    uint8_t buf[BDC_HDR_LEN + 2048];
    pthread_mutex_lock(&f->mu);
    bool up = (f->state == FW_UP || f->state == FW_AP) && f->host_enabled;
    pthread_mutex_unlock(&f->mu);
    size_t n = up && len + BDC_HDR_LEN <= HOST_RX_MAX ? bdc_wrap_data(buf, sizeof buf, 0, eth, len) : 0;
    if (!n) {
        f->rx_dropped++;
        return;
    }
    f->rx_frames++;
    f->to_host(buf, n, f->to_host_ctx);
}

void fw_host_data(struct fw *f, const uint8_t *bdc, size_t len)
{
    uint8_t ifidx;
    const uint8_t *eth;
    size_t eth_len;
    if (bdc_unwrap(bdc, len, &ifidx, &eth, &eth_len) < 0) {
        LOGW(TAG, "malformed BDC frame from host (%zu bytes)", len);
        return;
    }
    pthread_mutex_lock(&f->mu);
    bool up = f->state == FW_UP || f->state == FW_AP;
    pthread_mutex_unlock(&f->mu);
    if (!up) {
        f->tx_dropped++;
        return;
    }
    f->tx_frames++;
    f->be->ops->tx(f->be, eth, eth_len);
}

void fw_host_enable(struct fw *f, bool enabled)
{
    pthread_mutex_lock(&f->mu);
    bool was = f->host_enabled;
    f->host_enabled = enabled;
    if (was && !enabled) {
        /* The PC went away: give the radio back rather than staying associated for nobody. */
        f->scan_active = false;
        if (f->state != FW_IDLE) {
            radio_release(f);
            f->state = FW_IDLE;
        }
        f->have_pmk = f->have_pass = f->have_sae = false;
    }
    pthread_mutex_unlock(&f->mu);
}

/* ---------------------------------------------------------------- hotspot */

/* The association request the driver is told about when a station joins: the driver only passes these elements to
 * cfg80211, and the PC's hostapd checks them against its configuration. The Pi's radio did the real association, so they
 * are rebuilt from what this hotspot offers. */
static size_t ap_assoc_ies(const struct fw *f, uint8_t *out, size_t cap)
{
    static const uint8_t rates[] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
    static const uint8_t rsn[] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };
    size_t n = 0, need = 2 + f->ap.ssid_len + 2 + sizeof rates + (f->ap.wpa2_psk ? sizeof rsn : 0);
    if (need > cap)
        return 0;
    out[n++] = 0; out[n++] = f->ap.ssid_len;
    memcpy(out + n, f->ap.ssid, f->ap.ssid_len); n += f->ap.ssid_len;
    out[n++] = 1; out[n++] = sizeof rates;
    memcpy(out + n, rates, sizeof rates); n += sizeof rates;
    if (f->ap.wpa2_psk) {
        memcpy(out + n, rsn, sizeof rsn);
        n += sizeof rsn;
    }
    return n;
}

static int ap_set_ssid(struct fw *f, const uint8_t *ssid, uint32_t ssid_len)
{
    if (ssid_len == 0) {                                  /* the driver tears the hotspot down this way */
        if (f->state == FW_AP) {
            LOGI(TAG, "hotspot stopped by the host");
            f->state = FW_IDLE;
            f->n_sta = 0;
            if (f->be->ops->ap_stop)
                f->be->ops->ap_stop(f->be);
        }
        return BCME_OK;
    }
    if (ssid_len > 32)
        return BCME_BADLEN;
    if (!f->be->ops->ap_start) {
        LOGW(TAG, "hotspot refused: this backend cannot run one");
        return BCME_UNSUPPORTED;
    }
    struct wl_ap_req r;
    memset(&r, 0, sizeof r);
    r.ssid_len = (uint8_t)ssid_len;
    memcpy(r.ssid, ssid, ssid_len);
    struct fw_kv *k = kv_find(f, "chanspec");
    r.channel = k && k->len >= 2 ? (uint8_t)CHSPEC_CH(rd16(k->val)) : 0;
    if (r.channel < 1 || r.channel > 13)
        r.channel = 6;
    k = kv_find(f, "closednet");
    r.hidden = k && k->len >= 4 && rd32(k->val) != 0;
    r.beacon_interval = f->ioctl_set[WLC_SET_BCNPRD] ? (uint16_t)f->ioctl_int[WLC_SET_BCNPRD] : 0;
    r.dtim_period = f->ioctl_set[WLC_SET_DTIMPRD] ? (uint8_t)f->ioctl_int[WLC_SET_DTIMPRD] : 0;
    /* wpa_auth 0 = open; WPA2-PSK = 0x80. Anything else (WPA, WPA3, 802.1X, WEP) is not offered. */
    if (f->wpa_auth == 0 && f->wsec == 0) {
        r.wpa2_psk = false;
    } else if ((f->wpa_auth & ~0x8080u) == 0 && (f->wpa_auth & 0x8080) && (f->wsec & 4) && (f->have_pmk || f->have_pass) && f->mfp <= 2) {
        r.wpa2_psk = true;
        r.psk_sha256 = f->wpa_auth & 0x8000;
        r.pmf = (uint8_t)f->mfp;
        r.have_pmk = f->have_pmk;
        memcpy(r.pmk, f->pmk, 32);
        r.have_passphrase = f->have_pass;
        memcpy(r.passphrase, f->pass, sizeof r.passphrase);
    } else {
        LOGW(TAG, "hotspot refused: only open and WPA2-PSK (AES) are supported (wpa_auth=%#x wsec=%#x pmk=%d)",
             f->wpa_auth, f->wsec, f->have_pmk || f->have_pass);
        return BCME_UNSUPPORTED;
    }
    LOGI(TAG, "hotspot \"%.*s\" on channel %u, %s%s", r.ssid_len, r.ssid, r.channel, r.wpa2_psk ? "WPA2-PSK" : "open",
         r.hidden ? ", hidden" : "");
    f->ap = r;
    f->n_sta = 0;
    f->state = FW_AP;
    f->have_bss = false;
    f->be->ops->ap_start(f->be, &r);
    return BCME_OK;
}

static int sta_info_reply(struct fw *f, int i, struct out *o)
{
    /* struct brcmf_sta_info_le, version 4: only what the driver reads for a station that is connected and authorized. */
    uint8_t b[160] = { 0 };
    b[0] = 4;                                            /* ver */
    /* The kernel declares the struct without packing: ver@0 len@2 cap@4, then 4-byte aligned flags@8 idle@12 ea@16
     * count@24 rates[16]@28 in@44. */
    b[2] = sizeof b & 0xff; b[3] = sizeof b >> 8;        /* len */
    wr32(b + 8, 0x02 | 0x08 | 0x10 | 0x20);              /* WME | AUTHE | ASSOC | AUTHO */
    wr32(b + 12, 0);                                     /* idle: always active (hostapd must not poll it away) */
    memcpy(b + 16, f->sta[i].mac, 6);
    wr32(b + 44, (uint32_t)((now_ms() - f->sta[i].since_ms) / 1000));   /* "in": seconds since association */
    size_t n = o->cap < sizeof b ? o->cap : sizeof b;
    return out_put(o, b, n);
}

void fw_ap_station(struct fw *f, const uint8_t mac[6], bool connected)
{
    pthread_mutex_lock(&f->mu);
    if (f->state != FW_AP) {
        pthread_mutex_unlock(&f->mu);
        return;
    }
    int idx = -1;
    for (int i = 0; i < f->n_sta; i++)
        if (!memcmp(f->sta[i].mac, mac, 6))
            idx = i;
    if (connected) {
        if (idx < 0 && f->n_sta < (int)(sizeof f->sta / sizeof *f->sta)) {
            idx = f->n_sta++;
            memcpy(f->sta[idx].mac, mac, 6);
        }
        if (idx >= 0)
            f->sta[idx].since_ms = now_ms();
        uint8_t ies[96];
        size_t n = ap_assoc_ies(f, ies, sizeof ies);
        LOGI(TAG, "hotspot: station %02x:%02x:%02x:%02x:%02x:%02x connected", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        emit(f, EVT_ASSOC_IND, EVTST_SUCCESS, 0, 0, mac, ies, n);
    } else {
        if (idx >= 0)
            f->sta[idx] = f->sta[--f->n_sta];
        LOGI(TAG, "hotspot: station %02x:%02x:%02x:%02x:%02x:%02x left", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        emit(f, EVT_DISASSOC_IND, EVTST_SUCCESS, 0, 0, mac, NULL, 0);
    }
    pthread_mutex_unlock(&f->mu);
}

/* ---------------------------------------------------------------- iovars */

static const uint8_t k_bcast[6] = { 255, 255, 255, 255, 255, 255 };

static int var_get(struct fw *f, const char *name, const uint8_t *in, size_t inlen, struct out *o)
{
    if (!strcmp(name, "cur_etheraddr"))
        return out_put(o, f->mac, 6);
    if (!strcmp(name, "ver"))
        return out_str(o, "wl0: Nov  9 2026 00:00:00 version 7.45.98.0 (pi-radio-gadget emulated FullMAC) FWID 01-00000000");
    if (!strcmp(name, "cap"))
        return out_str(o, f->advertise_sae ? "sta idauth sae " : "sta idauth ");
    if (!strcmp(name, "event_msgs"))                  /* the mask the host set (all zero initially) */
        return out_put(o, f->event_mask, o->cap < sizeof f->event_mask ? o->cap : sizeof f->event_mask);
    if (!strcmp(name, "per_chan_info"))               /* WL_CHAN_VALID_HW | WL_CHAN_VALID_SW; no radar/passive flags */
        return out_u32(o, 3);
    if (!strcmp(name, "chanspec"))                    /* current channel (any valid one while idle) */
        return out_u32(o, CHSPEC_2G_20(f->state == FW_UP ? f->cur_bss.channel : f->state == FW_AP ? f->ap.channel : 1));
    if (!strcmp(name, "qtxpower"))                    /* quarter-dBm: 20 dBm */
        return out_u32(o, 80);
    /* Tables the driver reads after association; "nothing configured" is a valid answer (not an error). */
    if (!strcmp(name, "arp_hostip") || !strcmp(name, "nd_hostip") || !strcmp(name, "wme_ac_sta")) {
        o->len = o->cap;                            /* the data area is zero-filled already */
        return BCME_OK;
    }
    if (!strcmp(name, "nmode"))
        return out_u32(o, 1);
    if (!strcmp(name, "vhtmode"))
        return out_u32(o, 0);
    if (!strcmp(name, "rxchain"))
        return out_u32(o, 1);
    if (!strcmp(name, "bw_cap"))
        return out_u32(o, 1);                         /* 20 MHz bit, either band */
    if (!strcmp(name, "chanspecs")) {
        /* The host may pass a chanspec as a filter (band/bandwidth). We only have 2.4 GHz 20 MHz
         * channels, so a 40 MHz query matches nothing. */
        uint16_t filt = inlen >= 2 ? rd16(in) : 0;
        if (filt & 0x0c00 && (filt & 0x0c00) != 0x0800)
            return out_u32(o, 0);
        int rc = out_u32(o, 13);
        for (unsigned ch = 1; ch <= 13 && !rc; ch++)
            rc = out_u32(o, CHSPEC_2G_20(ch));
        return rc;
    }
    if (!strcmp(name, "country")) {                   /* brcmf_fil_country_le: abbrev[4] rev ccode[4] */
        struct kv_dummy { char a[4]; uint8_t rev[4]; char c[4]; } c = { "XZ", { 0xff, 0xff, 0xff, 0xff }, "XZ" };
        struct fw_kv *k = kv_find(f, "country");
        return k ? out_put(o, k->val, k->len) : out_put(o, &c, sizeof c);
    }
    if (!strcmp(name, "assoc_info")) {                /* req_len, resp_len */
        if (f->state != FW_UP || !f->have_bss)
            return BCME_NOTASSOCIATED;
        int rc = out_u32(o, 0);                       /* the host builds its own request IEs */
        return rc ? rc : out_u32(o, f->cur_bss.ie_len);
    }
    if (!strcmp(name, "assoc_req_ies"))
        return BCME_OK;
    if (!strcmp(name, "assoc_resp_ies")) {            /* closest thing we have: the AP's own IEs */
        if (f->state != FW_UP || !f->have_bss)
            return BCME_NOTASSOCIATED;
        return out_put(o, f->cur_bss.ie, f->cur_bss.ie_len);
    }
    if (!strcmp(name, "sta_info")) {
        if (f->state != FW_AP || inlen < 6)
            return BCME_UNSUPPORTED;
        for (int i = 0; i < f->n_sta; i++)
            if (!memcmp(f->sta[i].mac, in, 6))
                return sta_info_reply(f, i, o);
        return BCME_NOTASSOCIATED;
    }

    /* Probed-for features that we deliberately do not have must stay "unsupported". */
    struct fw_kv *k = kv_find(f, name);
    if (k) {
        size_t n = k->len < o->cap ? k->len : o->cap;
        return out_put(o, k->val, n);
    }
    (void)in; (void)inlen;
    LOGD(TAG, "get iovar \"%s\": unsupported", name);
    return BCME_UNSUPPORTED;
}

static int var_set(struct fw *f, const char *name, const uint8_t *d, size_t n)
{
    if (!strcmp(name, "event_msgs")) {
        memset(f->event_mask, 0, sizeof f->event_mask);
        memcpy(f->event_mask, d, n < sizeof f->event_mask ? n : sizeof f->event_mask);
    } else if (!strcmp(name, "escan")) {
        parse_escan(f, d, n);
        return BCME_OK;
    } else if (!strcmp(name, "join")) {
        /* ext_join: ssid_le(36) scan(20) assoc{bssid(6) pad(2) chanspec_num(4) chanspec_list[]} */
        if (n < 68)
            return BCME_BADLEN;
        uint32_t chans = rd32(d + 64);
        uint8_t ch = chans && n >= 70 ? (uint8_t)CHSPEC_CH(rd16(d + 68)) : 0;
        start_join(f, d + 4, rd32(d), d + 56, ch);
        return BCME_OK;
    } else if (!strcmp(name, "sup_wpa")) {
        f->sup_wpa = n >= 4 ? rd32(d) : 0;
    } else if (!strcmp(name, "wpa_auth")) {
        f->wpa_auth = n >= 4 ? rd32(d) : 0;
    } else if (!strcmp(name, "auth")) {
        f->auth = n >= 4 ? rd32(d) : 0;
    } else if (!strcmp(name, "wsec")) {
        f->wsec = n >= 4 ? rd32(d) : 0;
        if (!f->wsec)
            f->have_pmk = f->have_pass = f->have_sae = false;
    } else if (!strcmp(name, "mfp")) {
        f->mfp = n >= 4 ? rd32(d) : 0;
    } else if (!strcmp(name, "sae_password")) {
        if (f->ap_mode) {
            LOGW(TAG, "WPA3-SAE hotspot refused: the Pi's chip cannot do it (use WPA2-PSK or open)");
            return BCME_UNSUPPORTED;
        }
        if (n >= 2) {
            unsigned l = rd16(d);
            if (l > 128 || l + 2 > n)
                return BCME_BADLEN;
            memcpy(f->sae_pw, d + 2, l);
            f->sae_pw[l] = 0;
            f->have_sae = l > 0;
        }
    } else if (!strcmp(name, "cur_etheraddr") && n >= 6) {
        if (memcmp(d, f->mac, 6) != 0) {
            LOGI(TAG, "host changed the MAC address to %02x:%02x:%02x:%02x:%02x:%02x", d[0], d[1], d[2], d[3], d[4], d[5]);
            memcpy(f->mac, d, 6);
            if (f->be->ops->set_mac)
                f->be->ops->set_mac(f->be, d);
        }
    } else if (!strcmp(name, "country") && n >= 12) {
        /* brcmf_fil_country_le: abbrev[4] rev[4] ccode[4]. "XZ"/"ww"/digits = world or unset: nothing to apply. */
        char cc[3] = { (char)d[8], (char)d[9], 0 };
        if (cc[0] >= 'A' && cc[0] <= 'Z' && cc[1] >= 'A' && cc[1] <= 'Z' && strcmp(cc, "XZ") != 0) {
            LOGI(TAG, "host selected regulatory domain %s", cc);
            if (f->be->ops->set_country)
                f->be->ops->set_country(f->be, cc);
        }
    } else if (!strcmp(name, "escan") || !strcmp(name, "interface_create")) {
        return BCME_UNSUPPORTED;
    } else {
        LOGD(TAG, "set iovar \"%s\" (%zu bytes) stored", name, n);
    }
    kv_set(f, name, d, n);
    return BCME_OK;
}

static int do_var(struct fw *f, bool set, const uint8_t *pl, size_t plen, struct out *o)
{
    size_t nlen = strnlen((const char *)pl, plen);
    if (nlen == plen)
        return BCME_BADARG;
    char name[48];
    snprintf(name, sizeof name, "%s", (const char *)pl);
    const uint8_t *data = pl + nlen + 1;
    size_t dlen = plen - nlen - 1;

    const char *nm = name;
    if (!strncmp(nm, "bsscfg:", 7)) {                 /* bsscfg:<name>: first 4 bytes = bss index */
        nm += 7;
        if (dlen < 4)
            return BCME_BADLEN;
        data += 4;
        dlen -= 4;
    }
    return set ? var_set(f, nm, data, dlen) : var_get(f, nm, data, dlen, o);
}

/* ---------------------------------------------------------------- ioctls */

static int do_ioctl(struct fw *f, uint32_t cmd, bool set, const uint8_t *in, size_t inlen, struct out *o)
{
    if (cmd >= 511)
        return BCME_UNSUPPORTED;

    switch (cmd) {
    case WLC_GET_VERSION:     return out_u32(o, 1);   /* d11 "io type" 1 = N: selects the chanspec encoding */
    case WLC_UP: case WLC_DOWN: return BCME_OK;
    case WLC_GET_REVINFO: {
        uint32_t r[17] = { 0x14e4, 0x4330, 0x10, 1, 0x20, 0, 0x14e4, 1, 0x01000000, 0x01000000,
                           2 /* USB */, 43143, 4, 1, 1, 0, 0 };
        for (int i = 0; i < 17; i++)
            if (out_u32(o, r[i]))
                return BCME_BADLEN;
        return BCME_OK;
    }
    case WLC_GET_BANDLIST:                            /* count, then bands: 2 = 2.4 GHz */
        return out_u32(o, 1) ? BCME_BADLEN : out_u32(o, 2);
    case WLC_GET_PHYLIST:     return out_str(o, "n");
    case WLC_GET_BSSID:
        if (f->state != FW_UP)
            return BCME_NOTASSOCIATED;
        return out_put(o, f->cur_bss.bssid, 6);
    case WLC_GET_SSID: {
        if (f->state != FW_UP)
            return BCME_NOTASSOCIATED;
        uint8_t s[36] = { 0 };
        wr32(s, f->cur_bss.ssid_len);
        memcpy(s + 4, f->cur_bss.ssid, f->cur_bss.ssid_len);
        return out_put(o, s, 36);
    }
    case WLC_GET_BSS_INFO: {                          /* u32 length prefix, then bss_info_le */
        if (f->state != FW_UP || !f->have_bss)
            return BCME_NOTASSOCIATED;
        uint8_t tmp[BSS_INFO_MAX];
        size_t n = fw_build_bss_info(tmp, sizeof tmp, &f->cur_bss);
        if (!n || o->cap < 4 + n)
            return BCME_BADLEN;
        out_u32(o, (uint32_t)n);
        return out_put(o, tmp, n);
    }
    case WLC_GET_RSSI: {                              /* brcmf_scb_val_le: val, ea */
        struct wl_link l;
        int rssi = -70;
        if (f->be->ops->link && f->be->ops->link(f->be, &l) && l.up)
            rssi = l.rssi;
        return out_u32(o, (uint32_t)(int32_t)(f->state == FW_UP ? rssi : 0));
    }
    case WLC_GET_PHY_NOISE:   return out_u32(o, (uint32_t)(int32_t)-90);
    case WLC_GET_CHANNEL: {                           /* hw, target, scan channel */
        uint32_t ch = f->state == FW_UP ? f->cur_bss.channel : f->state == FW_AP ? f->ap.channel : 0;
        return (out_u32(o, ch) || out_u32(o, ch) || out_u32(o, 0)) ? BCME_BADLEN : BCME_OK;
    }
    case WLC_GET_RATE: {
        struct wl_link l;
        uint32_t kbps = 65000;
        if (f->state == FW_UP && f->be->ops->link && f->be->ops->link(f->be, &l) && l.up && l.rate_kbps)
            kbps = l.rate_kbps;
        return out_u32(o, kbps / 500);
    }
    case WLC_GET_ASSOCLIST: {                         /* u32 count, then the MACs */
        int n = f->state == FW_AP ? f->n_sta : 0;
        if (out_u32(o, (uint32_t)n))
            return BCME_BADLEN;
        for (int i = 0; i < n; i++)
            if (out_put(o, f->sta[i].mac, 6))
                return BCME_BADLEN;
        return BCME_OK;
    }
    case WLC_SCB_DEAUTHENTICATE_FOR_REASON: {         /* scb_val: reason, MAC */
        if (inlen < 10)
            return BCME_BADLEN;
        if (f->state == FW_AP && f->be->ops->ap_deauth)
            f->be->ops->ap_deauth(f->be, in + 4, (uint16_t)rd32(in));
        return BCME_OK;
    }
    case WLC_SET_SCB_AUTHORIZE: case WLC_SET_SCB_DEAUTHORIZE:
        return BCME_OK;                                /* the chip authorizes by itself (PSK offload) */
    case WLC_GET_GET_PKTCNTS: return o->cap >= 40 ? (o->len = 40, BCME_OK) : BCME_BADLEN;
    case WLC_SET_SSID: {                              /* legacy join: ssid_le(36) assoc{bssid, pad, n, list} */
        if (inlen < 36)
            return BCME_BADLEN;
        if (f->ap_mode)
            return ap_set_ssid(f, in + 4, rd32(in));
        uint8_t ch = (inlen >= 36 + 12 + 2 && rd32(in + 36 + 8) > 0) ? (uint8_t)CHSPEC_CH(rd16(in + 36 + 12)) : 0;
        start_join(f, in + 4, rd32(in), inlen >= 42 ? in + 36 : k_bcast, ch);
        return BCME_OK;
    }
    case WLC_SET_WSEC_PMK: {
        if (inlen < 4)
            return BCME_BADLEN;
        unsigned l = rd16(in), fl = rd16(in + 2);
        if (4 + l > inlen)
            return BCME_BADLEN;
        if (l == 0) {                                  /* the driver clears the key between connections */
            f->have_pmk = f->have_pass = false;
            return BCME_OK;
        }
        if (fl & WSEC_PASSPHRASE) {
            if (l > 64)
                return BCME_BADLEN;
            memcpy(f->pass, in + 4, l);
            f->pass[l] = 0;
            f->have_pass = true;
        } else if (l == 32) {
            memcpy(f->pmk, in + 4, 32);
            f->have_pmk = true;
        } else {
            return BCME_BADLEN;
        }
        return BCME_OK;
    }
    case WLC_DISASSOC: {
        LOGI(TAG, "host requested disassociation");
        bool was_up = f->state != FW_IDLE;
        f->state = FW_IDLE;
        f->have_pmk = f->have_pass = f->have_sae = false;
        if (was_up) {
            f->be->ops->leave(f->be);
            emit(f, EVT_LINK, EVTST_SUCCESS, 0, 0, f->cur_bss.bssid, NULL, 0);
        }
        return BCME_OK;
    }
    case WLC_SCAN:            return BCME_UNSUPPORTED;     /* legacy scan: the driver uses escan */
    case WLC_SET_AP:                                  /* the driver switches the interface to access-point mode */
        f->ap_mode = inlen >= 4 && rd32(in) != 0;
        break;
    case WLC_SET_WSEC:
        f->wsec = inlen >= 4 ? rd32(in) : 0;
        break;
    case WLC_GET_WSEC:
        return out_u32(o, f->wsec);
    default:
        break;
    }

    /* Everything else is a plain "set remembers an int, get returns it" ioctl. */
    if (set) {
        if (inlen >= 4) {
            f->ioctl_int[cmd] = rd32(in);
            f->ioctl_set[cmd] = true;
        }
        return BCME_OK;
    }
    /* Broadcom numbers get/set ioctls as pairs (GET_PM 85 / SET_PM 86): a get returns what its set partner stored. */
    if (f->ioctl_set[cmd + 1])
        return out_u32(o, f->ioctl_int[cmd + 1]);
    if (!f->ioctl_set[cmd])
        LOGD(TAG, "get ioctl %u: returning 0", cmd);
    return out_u32(o, f->ioctl_int[cmd]);
}

/* ---------------------------------------------------------------- entry */

size_t fw_dcmd(struct fw *f, const uint8_t *req, size_t req_len, uint8_t *resp, size_t resp_cap)
{
    if (req_len < sizeof(struct dcmd_hdr) || resp_cap < sizeof(struct dcmd_hdr))
        return 0;
    uint32_t cmd = rd32(req), len = rd32(req + 4), flags = rd32(req + 8);
    const uint8_t *pl = req + sizeof(struct dcmd_hdr);
    size_t plen = req_len - sizeof(struct dcmd_hdr);
    if (len > plen)
        len = (uint32_t)plen;                          /* the host may send less than it asked to read */
    size_t datacap = resp_cap - sizeof(struct dcmd_hdr);
    size_t want = rd32(req + 4);                       /* the host reads this many bytes back */
    if (want > DCMD_MAXLEN)
        want = DCMD_MAXLEN;
    if (want > datacap)
        want = datacap;

    uint8_t *data = resp + sizeof(struct dcmd_hdr);
    memset(data, 0, want);
    struct out o = { .p = data, .cap = want };

    bool set = flags & DCMD_SET;
    pthread_mutex_lock(&f->mu);
    int st;
    if (cmd == WLC_GET_VAR || cmd == WLC_SET_VAR)
        st = do_var(f, cmd == WLC_SET_VAR, pl, len, &o);
    else
        st = do_ioctl(f, cmd, set, pl, len, &o);
    pthread_mutex_unlock(&f->mu);

    if (log_level() >= LOG_DBG) {
        char hx[40];
        hex_prefix(pl, len, 16, hx, sizeof hx);
        if (cmd == WLC_GET_VAR || cmd == WLC_SET_VAR)
            LOGD(TAG, "%s \"%.40s\" len=%u -> %d", set ? "SET_VAR" : "GET_VAR", (const char *)pl, len, st);
        else
            LOGD(TAG, "%s ioctl %u len=%u %s -> %d", set ? "SET" : "GET", cmd, len, hx, st);
    }

    struct dcmd_hdr h;
    wr32((uint8_t *)&h.cmd, cmd);
    wr32((uint8_t *)&h.len, (uint32_t)want);
    uint32_t rflags = (flags & ~DCMD_ERROR) | (st ? DCMD_ERROR : 0);
    wr32((uint8_t *)&h.flags, rflags);
    wr32((uint8_t *)&h.status, (uint32_t)(int32_t)st);
    memcpy(resp, &h, sizeof h);
    return sizeof h + want;
}

void fw_init(struct fw *f, const uint8_t mac[6], struct wl_backend *be, fw_to_host_fn to_host, void *ctx)
{
    memset(f, 0, sizeof *f);
    pthread_mutex_init(&f->mu, NULL);
    memcpy(f->mac, mac, 6);
    f->be = be;
    be->fw = f;
    f->to_host = to_host;
    f->to_host_ctx = ctx;
    f->state = FW_IDLE;
    /* Probed with "get": must exist for the driver to enable PSK offload and PMF. */
    kv_set_u32(f, "sup_wpa", 0);
    kv_set_u32(f, "mfp", 0);
    kv_set_u32(f, "mpc", 1);
}

void fw_destroy(struct fw *f)
{
    pthread_mutex_destroy(&f->mu);
}

void fw_reset(struct fw *f)
{
    pthread_mutex_lock(&f->mu);
    if (f->state != FW_IDLE)
        radio_release(f);
    f->state = FW_IDLE;
    f->scan_active = false;
    f->have_bss = false;
    f->have_pmk = f->have_pass = f->have_sae = false;
    f->ap_mode = false;
    f->wsec = f->wpa_auth = f->auth = f->mfp = f->sup_wpa = 0;
    memset(f->event_mask, 0, sizeof f->event_mask);
    memset(f->ioctl_set, 0, sizeof f->ioctl_set);
    f->n_kv = 0;
    kv_set_u32(f, "sup_wpa", 0);
    kv_set_u32(f, "mfp", 0);
    kv_set_u32(f, "mpc", 1);
    pthread_mutex_unlock(&f->mu);
}

void fw_ap_lost(struct fw *f)
{
    pthread_mutex_lock(&f->mu);
    if (f->state == FW_AP) {
        LOGW(TAG, "hotspot lost");
        f->state = FW_IDLE;
        f->n_sta = 0;
    }
    pthread_mutex_unlock(&f->mu);
}

size_t fw_stats_line(struct fw *f, char *buf, size_t size)
{
    static const char *const names[] = { "idle", "joining", "station", "hotspot" };
    char be[160] = "";
    if (f->be && f->be->ops->status)
        f->be->ops->status(f->be, be, sizeof be);
    pthread_mutex_lock(&f->mu);
    int n = snprintf(buf, size, "wifi[%s stations=%d to_host=%lu from_host=%lu drop_to_host=%lu drop_from_host=%lu] %s",
                     names[f->state], f->n_sta, f->rx_frames, f->tx_frames, f->rx_dropped, f->tx_dropped, be);
    pthread_mutex_unlock(&f->mu);
    return n < 0 ? 0 : ((size_t)n >= size ? size - 1 : (size_t)n);
}
