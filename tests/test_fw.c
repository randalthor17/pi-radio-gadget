/* Drives the emulated firmware the way brcmfmac does, byte for byte, with a recording backend. */
#include <endian.h>
#include "bdc.h"
#include "check.h"
#include "fw.h"

/* ---- recording backend */
static struct {
    int scans, joins, leaves, txs, aborts;
    struct wl_scan_req scan;
    struct wl_join_req join;
} rec;

static void be_scan(struct wl_backend *b, const struct wl_scan_req *r) { (void)b; rec.scans++; rec.scan = *r; }
static void be_abort(struct wl_backend *b) { (void)b; rec.aborts++; }
static void be_join(struct wl_backend *b, const struct wl_join_req *r) { (void)b; rec.joins++; rec.join = *r; }
static void be_leave(struct wl_backend *b) { (void)b; rec.leaves++; }
static void be_tx(struct wl_backend *b, const uint8_t *e, size_t n) { (void)b; (void)e; (void)n; rec.txs++; }
static bool be_link(struct wl_backend *b, struct wl_link *l)
{
    (void)b;
    memset(l, 0, sizeof *l);
    l->up = true; l->rssi = -55; l->rate_kbps = 72200;
    return true;
}
static struct { int starts, stops, deauths; struct wl_ap_req req; uint8_t sta[6]; uint16_t reason; } ap;
static void be_ap_start(struct wl_backend *b, const struct wl_ap_req *r) { (void)b; ap.starts++; ap.req = *r; }
static void be_ap_stop(struct wl_backend *b) { (void)b; ap.stops++; }
static void be_ap_deauth(struct wl_backend *b, const uint8_t *m, uint16_t why) { (void)b; ap.deauths++; memcpy(ap.sta, m, 6); ap.reason = why; }
static const struct wl_backend_ops ops = {
    .name = "rec", .scan = be_scan, .scan_abort = be_abort, .join = be_join, .leave = be_leave,
    .tx = be_tx, .link = be_link,
    .ap_start = be_ap_start, .ap_stop = be_ap_stop, .ap_deauth = be_ap_deauth,
};

/* ---- frames delivered to the host */
static struct { uint8_t d[4300]; size_t n; } host[32];
static int nhost;
static void to_host(const uint8_t *b, size_t n, void *ctx)
{
    (void)ctx;
    if (nhost < 32 && n <= sizeof host[0].d) { memcpy(host[nhost].d, b, n); host[nhost].n = n; nhost++; }
}

/* event accessors: BDC(4) eth(14) brcm(10) msg(48) data */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t ev_code(int i)   { return be32(host[i].d + 4 + 14 + 10 + 4); }
static uint32_t ev_status(int i) { return be32(host[i].d + 4 + 14 + 10 + 8); }

/* ---- dcmd helpers */
static uint32_t g_id;

static size_t mkreq(uint8_t *out, uint32_t cmd, bool set, const void *pl, size_t plen, size_t buflen)
{
    uint32_t h[4] = { htole32(cmd), htole32((uint32_t)buflen), htole32((set ? 2u : 0u) | (++g_id << 16)), 0 };
    memcpy(out, h, 16);
    memset(out + 16, 0, buflen);
    memcpy(out + 16, pl, plen);
    return 16 + buflen;
}

static size_t mkvar(uint8_t *out, bool set, const char *name, const void *d, size_t dlen, size_t buflen)
{
    uint8_t pl[512];
    size_t nl = strlen(name) + 1;
    memcpy(pl, name, nl);
    if (dlen) memcpy(pl + nl, d, dlen);
    return mkreq(out, set ? WLC_SET_VAR : WLC_GET_VAR, set, pl, nl + dlen, buflen > nl + dlen ? buflen : nl + dlen);
}

struct resp { uint8_t b[DCMD_MAXLEN + 64]; size_t n; int32_t status; bool err; uint32_t id; };

static void run(struct fw *f, const uint8_t *req, size_t n, struct resp *r)
{
    r->n = fw_dcmd(f, req, n, r->b, sizeof r->b);
    uint32_t flags = le32toh(*(uint32_t *)(r->b + 8));
    r->err = flags & 1;
    r->id = flags >> 16;
    r->status = (int32_t)le32toh(*(uint32_t *)(r->b + 12));
}

static uint32_t rd32(const uint8_t *p) { return le32toh(*(const uint32_t *)p); }

int main(void)
{
    const uint8_t mac[6] = { 0xb8, 0x27, 0xeb, 0x11, 0x22, 0x33 };
    struct wl_backend be = { .ops = &ops };
    static struct fw f;
    fw_init(&f, mac, &be, to_host, NULL);
    uint8_t req[4096];
    static struct resp r;

    /* --- plain queries */
    run(&f, req, mkvar(req, false, "cur_etheraddr", NULL, 0, 6), &r);
    CHECK(!r.err && !memcmp(r.b + 16, mac, 6), "cur_etheraddr");
    CHECK(r.id == g_id, "response echoes the request id");

    run(&f, req, mkvar(req, false, "ver", NULL, 0, 256), &r);
    CHECK(!r.err && strstr((char *)r.b + 16, "FWID"), "ver string");
    const char *vs = (char *)r.b + 16;
    CHECK(strrchr(vs, ' ') != NULL, "driver takes the last word of ver as the version number");

    run(&f, req, mkvar(req, false, "cap", NULL, 0, 128), &r);
    CHECK(!r.err && strstr((char *)r.b + 16, "sta"), "cap");
    CHECK(!strstr((char *)r.b + 16, "p2p") && !strstr((char *)r.b + 16, "mbss"), "cap must not advertise p2p/mbss");

    run(&f, req, mkreq(req, WLC_GET_VERSION, false, NULL, 0, 4), &r);
    CHECK(!r.err && rd32(r.b + 16) == 1, "io type 1 (D11N)");

    run(&f, req, mkvar(req, false, "chanspecs", NULL, 0, 1536), &r);
    CHECK(!r.err && rd32(r.b + 16) == 13 && rd32(r.b + 20) == 0x2b01 && rd32(r.b + 16 + 4 * 13) == 0x2b0d,
          "chanspecs: 13 channels in D11N 20 MHz encoding (0x2b01..0x2b0d)");

    /* The 40 MHz probe passes a filter chanspec; 20 MHz-only hardware must answer with an empty list. */
    {
        uint16_t f40 = htole16(0x2000 | 0x0c00 | 0x0300);
        run(&f, req, mkvar(req, false, "chanspecs", &f40, 2, 1536), &r);
        CHECK(!r.err && rd32(r.b + 16) == 0, "40 MHz chanspec query returns no channels");
        run(&f, req, mkvar(req, false, "bsscfg:per_chan_info", (uint8_t[]){ 0, 0, 0, 0, 1, 0x2b, 0, 0 }, 8, 8), &r);
        CHECK(!r.err && rd32(r.b + 16) == 3, "per_chan_info must succeed (the driver returns the last query error)");
    }

    /* tables read after association: empty, not an error */
    run(&f, req, mkvar(req, false, "arp_hostip", NULL, 0, 32), &r);
    CHECK(!r.err, "arp_hostip readable");
    run(&f, req, mkvar(req, false, "wme_ac_sta", NULL, 0, 32), &r);
    CHECK(!r.err, "wme_ac_sta readable");

    /* probed features: sup_wpa/mfp exist, the rest must be unsupported */
    run(&f, req, mkvar(req, false, "sup_wpa", NULL, 0, 4), &r);
    CHECK(!r.err, "sup_wpa readable (enables PSK offload)");
    run(&f, req, mkvar(req, false, "tdls_enable", NULL, 0, 4), &r);
    CHECK(r.err && r.status == BCME_UNSUPPORTED, "tdls_enable unsupported");
    run(&f, req, mkvar(req, false, "scan_ver", NULL, 0, 4), &r);
    CHECK(r.err, "scan_ver unsupported (driver then uses v1 scan params)");

    /* set then get */
    uint32_t one = htole32(1);
    run(&f, req, mkvar(req, true, "mpc", &one, 4, 4), &r);
    CHECK(!r.err, "set mpc");
    uint32_t two = htole32(2);
    run(&f, req, mkreq(req, WLC_SET_PM, true, &two, 4, 4), &r);
    run(&f, req, mkreq(req, WLC_GET_PM, false, NULL, 0, 4), &r);
    CHECK(!r.err && rd32(r.b + 16) == 2, "ioctl set/get round trip");

    /* --- escan: events are only delivered when enabled AND unmasked */
    uint8_t mask[16] = { 0 };
    mask[EVT_ESCAN_RESULT / 8] |= 1 << (EVT_ESCAN_RESULT % 8);
    mask[EVT_SET_SSID / 8] |= 1 << (EVT_SET_SSID % 8);
    mask[EVT_LINK / 8] |= 1 << (EVT_LINK % 8);
    mask[EVT_PSK_SUP / 8] |= 1 << (EVT_PSK_SUP % 8);
    run(&f, req, mkvar(req, true, "event_msgs", mask, 16, 16), &r);
    CHECK(!r.err, "event_msgs");

    /* escan params v1: version, action=1, sync=0x1234, then params with 2 channels + 1 ssid */
    uint8_t ep[8 + 64 + 4 + 36];
    memset(ep, 0, sizeof ep);
    uint32_t ver = htole32(1); memcpy(ep, &ver, 4);
    ep[4] = 1; ep[6] = 0x34; ep[7] = 0x12;
    uint8_t *sp = ep + 8;
    sp[44] = 0;                                         /* bss_type */
    sp[45] = 0;                                         /* scan_type: active */
    uint32_t cn = htole32((1u << 16) | 2); memcpy(sp + 60, &cn, 4);
    uint16_t c1 = htole16(0x2b01), c6 = htole16(0x2b06);
    memcpy(sp + 64, &c1, 2); memcpy(sp + 66, &c6, 2);
    uint32_t sl = htole32(4); memcpy(sp + 68, &sl, 4); memcpy(sp + 72, "home", 4);
    run(&f, req, mkvar(req, true, "escan", ep, sizeof ep, sizeof ep), &r);
    CHECK(!r.err && rec.scans == 1, "escan reached the backend");
    CHECK(rec.scan.n_channels == 2 && rec.scan.channels[0] == 1 && rec.scan.channels[1] == 6, "channel list parsed");
    CHECK(rec.scan.n_ssids == 1 && rec.scan.ssids[0].len == 4 && !memcmp(rec.scan.ssids[0].ssid, "home", 4), "ssid parsed");

    /* host not enabled yet: result is swallowed */
    struct wl_bss bss;
    memset(&bss, 0, sizeof bss);
    memcpy(bss.bssid, "\x02\xaa\xbb\xcc\xdd\x01", 6);
    bss.ssid_len = 4; memcpy(bss.ssid, "home", 4);
    bss.channel = 6; bss.rssi = -50; bss.capability = 0x0431; bss.beacon_int = 100;
    uint8_t ies[] = { 0, 4, 'h', 'o', 'm', 'e', 3, 1, 6, 45, 2, 0, 0 };
    memcpy(bss.ie, ies, sizeof ies); bss.ie_len = sizeof ies;
    fw_scan_result(&f, &bss);
    CHECK(nhost == 0, "nothing delivered while the host is not enabled");

    fw_host_enable(&f, true);
    fw_scan_result(&f, &bss);
    CHECK(nhost == 1 && ev_code(0) == EVT_ESCAN_RESULT && ev_status(0) == EVTST_PARTIAL, "partial escan result event");
    {
        const uint8_t *data = host[0].d + 4 + 14 + 10 + 48;
        uint32_t datalen = be32(host[0].d + 4 + 14 + 10 + 20);
        uint32_t buflen = rd32(data);
        CHECK(buflen == datalen, "escan buflen (%u) == event datalen (%u)", buflen, datalen);
        CHECK(data[10] == 1 && data[11] == 0, "bss_count == 1");
        CHECK(data[8] == 0x34 && data[9] == 0x12, "sync_id echoed");
        const uint8_t *bi = data + 12;
        CHECK(rd32(bi + 4) == buflen - 12, "bss_info length == buflen - fixed header (driver checks this)");
        CHECK(rd32(bi) == 109, "bss_info version 109");
        CHECK(!memcmp(bi + 8, bss.bssid, 6), "bssid");
        CHECK(bi[18] == 4 && !memcmp(bi + 19, "home", 4), "ssid");
        CHECK(bi[88] == 6, "ctl_ch (offset 88) = 6");
        CHECK(le16toh(*(uint16_t *)(bi + 116)) == 128, "ie_offset = sizeof(bss_info_le)");
        CHECK(rd32(bi + 120) == sizeof ies && !memcmp(bi + 128, ies, sizeof ies), "ies appended");
    }
    fw_scan_done(&f, false);
    CHECK(nhost == 2 && ev_code(1) == EVT_ESCAN_RESULT && ev_status(1) == EVTST_SUCCESS, "final escan event");
    fw_scan_done(&f, false);
    CHECK(nhost == 2, "scan_done is idempotent");

    /* --- the host's receive buffers are 1600 bytes: a scan result with huge IEs must be trimmed to fit */
    {
        struct wl_bss big;
        memset(&big, 0, sizeof big);
        memcpy(big.bssid, "\x02\xaa\xbb\xcc\xdd\x09", 6);
        big.channel = 3; big.rssi = -60; big.ssid_len = 3; memcpy(big.ssid, "big", 3);
        size_t o = 0;
        while (o + 2 + 200 <= BSS_MAX_IE) {                   /* 200-byte vendor elements up to the maximum */
            big.ie[o] = 221; big.ie[o + 1] = 200;
            memset(big.ie + o + 2, 0x77, 200);
            o += 202;
        }
        big.ie_len = (uint16_t)o;                              /* 1414 bytes: more than a frame can carry */
        int before_n = nhost;
        f.scan_active = true;
        fw_scan_result(&f, &big);
        CHECK(nhost == before_n + 1, "big scan result still delivered");
        CHECK(host[before_n].n <= 1600, "event frame fits the host buffer (%zu bytes)", host[before_n].n);
        const uint8_t *bi = host[before_n].d + 4 + 14 + 10 + 48 + 12;
        uint32_t ielen = rd32(bi + 120);
        CHECK(ielen % 202 == 0 && ielen > 0 && ielen < o, "IEs cut on an element boundary (%u of %zu)", ielen, o);
        f.scan_active = false;
        nhost = before_n;
    }
    {
        uint8_t ie[] = { 0, 2, 'a', 'b', 221, 10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
        CHECK(fw_trim_ies(ie, sizeof ie, 100) == sizeof ie, "everything fits");
        CHECK(fw_trim_ies(ie, sizeof ie, 5) == 4, "second element does not fit: keep the first only");
        CHECK(fw_trim_ies(ie, sizeof ie, 3) == 0, "nothing fits");
        CHECK(fw_trim_ies(ie, 10, 100) == 4, "truncated final element dropped");
    }

    /* --- join with PMK offload */
    nhost = 0;
    uint32_t v;
    v = htole32(0x80); run(&f, req, mkvar(req, true, "wpa_auth", &v, 4, 4), &r);
    v = htole32(4);    run(&f, req, mkvar(req, true, "wsec", &v, 4, 4), &r);
    v = htole32(1);    run(&f, req, mkvar(req, true, "sup_wpa", &v, 4, 4), &r);
    uint8_t pmk[4 + 128]; memset(pmk, 0, sizeof pmk);
    pmk[0] = 32; pmk[1] = 0; pmk[2] = 0; pmk[3] = 0;                 /* len 32, flags 0 */
    for (int i = 0; i < 32; i++) pmk[4 + i] = (uint8_t)(0xc0 + i);
    run(&f, req, mkreq(req, WLC_SET_WSEC_PMK, true, pmk, sizeof pmk, sizeof pmk), &r);
    CHECK(!r.err, "set pmk");

    /* a zero-length key clears any previous PMK (the driver does this between connections) */
    {
        uint8_t clr[4 + 128] = { 0 };
        run(&f, req, mkreq(req, WLC_SET_WSEC_PMK, true, clr, sizeof clr, sizeof clr), &r);
        CHECK(!r.err, "clearing the PMK is accepted");
        run(&f, req, mkreq(req, WLC_SET_WSEC_PMK, true, pmk, sizeof pmk, sizeof pmk), &r);
        CHECK(!r.err, "setting it again afterwards works");
    }

    uint8_t jp[68 + 2];
    memset(jp, 0, sizeof jp);
    uint32_t ssl = htole32(4); memcpy(jp, &ssl, 4); memcpy(jp + 4, "home", 4);
    memcpy(jp + 56, bss.bssid, 6);
    uint32_t nchan = htole32(1); memcpy(jp + 64, &nchan, 4);
    uint16_t cs = htole16(0x2b06); memcpy(jp + 68, &cs, 2);
    run(&f, req, mkvar(req, true, "bsscfg:join", (uint8_t[]){ 0, 0, 0, 0 }, 0, 0), &r);   /* too short: rejected */
    CHECK(r.err, "short join rejected");
    {
        uint8_t pl[4 + sizeof jp];
        memset(pl, 0, 4); memcpy(pl + 4, jp, sizeof jp);
        run(&f, req, mkvar(req, true, "bsscfg:join", pl, sizeof pl, sizeof pl), &r);
    }
    CHECK(!r.err && rec.joins == 1, "join reached the backend");
    CHECK(rec.join.ssid_len == 4 && !memcmp(rec.join.ssid, "home", 4), "join ssid");
    CHECK(!rec.join.bssid_any && !memcmp(rec.join.bssid, bss.bssid, 6) && rec.join.channel == 6, "join bssid+channel");
    CHECK(rec.join.have_pmk && rec.join.pmk[0] == 0xc0 && rec.join.pmk[31] == 0xdf, "PMK handed to the backend");
    CHECK(rec.join.wpa_auth == 0x80 && rec.join.wsec == 4, "auth params carried over");

    fw_join_done(&f, WL_JOIN_OK, &bss);
    CHECK(nhost == 3, "three events on link up (got %d)", nhost);
    CHECK(ev_code(0) == EVT_PSK_SUP && ev_status(0) == EVTST_FWSUP_COMPLETED, "PSK_SUP completed first");
    CHECK(ev_code(1) == EVT_SET_SSID && ev_status(1) == EVTST_SUCCESS, "SET_SSID success");
    CHECK(ev_code(2) == EVT_LINK && (be32(host[2].d + 4 + 14 + 10) & 0xffff) == 1, "LINK with the link-up flag");

    run(&f, req, mkreq(req, WLC_GET_BSSID, false, NULL, 0, 6), &r);
    CHECK(!r.err && !memcmp(r.b + 16, bss.bssid, 6), "GET_BSSID while joined");
    run(&f, req, mkreq(req, WLC_GET_RSSI, false, NULL, 0, 12), &r);
    CHECK(!r.err && (int32_t)rd32(r.b + 16) == -55, "GET_RSSI from the backend");
    run(&f, req, mkreq(req, WLC_GET_BSS_INFO, false, (uint8_t[]){ 0, 8, 0, 0 }, 4, 2048), &r);
    CHECK(!r.err && rd32(r.b + 16) == 128 + sizeof ies, "GET_BSS_INFO: length prefix then bss_info");

    /* data path */
    uint8_t eth[60] = { 0 }, bdc[80];
    size_t bn = bdc_wrap_data(bdc, sizeof bdc, 0, eth, sizeof eth);
    fw_host_data(&f, bdc, bn);
    CHECK(rec.txs == 1, "host frame forwarded to the radio while joined");
    nhost = 0;
    fw_rx_frame(&f, eth, sizeof eth);
    CHECK(nhost == 1 && host[0].n == 64, "radio frame delivered with a BDC header");

    /* link loss */
    nhost = 0;
    fw_link_lost(&f, 2);
    CHECK(rec.leaves == 0, "link loss does not call leave");
    CHECK(nhost == 1 && ev_code(0) == EVT_LINK, "LINK down event (deauth_ind is masked here)");
    run(&f, req, mkreq(req, WLC_GET_BSSID, false, NULL, 0, 6), &r);
    CHECK(r.err && r.status == BCME_NOTASSOCIATED, "GET_BSSID after loss");
    fw_host_data(&f, bdc, bn);
    CHECK(rec.txs == 1 && f.tx_dropped == 1, "frames dropped while not associated");

    /* failed join */
    nhost = 0;
    run(&f, req, mkreq(req, WLC_SET_SSID, true, (uint8_t[]){ 4, 0, 0, 0, 'n', 'o', 'p', 'e' }, 8, 48), &r);
    CHECK(!r.err && rec.joins == 2, "legacy SET_SSID join");
    fw_join_done(&f, WL_JOIN_NO_NETWORK, NULL);
    CHECK(nhost == 1 && ev_code(0) == EVT_SET_SSID && ev_status(0) == EVTST_NO_NETWORKS, "NO_NETWORKS reported");

    /* host disappearing releases the radio */
    run(&f, req, mkreq(req, WLC_SET_SSID, true, (uint8_t[]){ 4, 0, 0, 0, 'h', 'o', 'm', 'e' }, 8, 48), &r);
    fw_join_done(&f, WL_JOIN_OK, &bss);
    int before = rec.leaves;
    fw_host_enable(&f, false);
    CHECK(rec.leaves == before + 1, "radio released when the USB host goes away");

    /* a fresh probe starts clean */
    fw_reset(&f);
    run(&f, req, mkvar(req, false, "sup_wpa", NULL, 0, 4), &r);
    CHECK(!r.err && rd32(r.b + 16) == 0, "reset restores defaults");

    /* --- hotspot: the exact dialogue brcmf_cfg80211_start_ap() / stop_ap() has with the firmware */
    {
        fw_reset(&f);
        fw_host_enable(&f, true);
        uint8_t mask[32] = { 0 };
        mask[8 / 8] |= 1 << (8 % 8);                 /* ASSOC_IND */
        mask[12 / 8] |= 1 << (12 % 8);               /* DISASSOC_IND */
        run(&f, req, mkvar(req, true, "event_msgs", mask, sizeof mask, 0), &r);
        CHECK(!r.err, "event mask set");
        run(&f, req, mkvar(req, false, "cap", NULL, 0, 128), &r);
        CHECK(strstr((char *)r.b + 16, "idauth"), "cap advertises idauth (AP PSK offload)");

        uint32_t v;
        v = htole32(1); run(&f, req, mkreq(req, WLC_SET_BCNPRD, true, &v, 4, 4), &r); CHECK(!r.err, "bcnprd");
        v = htole32(0); run(&f, req, mkreq(req, WLC_DOWN, true, &v, 4, 4), &r); CHECK(!r.err, "down");
        v = htole32(0); run(&f, req, mkvar(req, true, "apsta", &v, 4, 0), &r); CHECK(!r.err, "apsta");
        v = htole32(1); run(&f, req, mkreq(req, WLC_SET_INFRA, true, &v, 4, 4), &r); CHECK(!r.err, "infra");
        v = htole32(1); run(&f, req, mkreq(req, WLC_SET_AP, true, &v, 4, 4), &r); CHECK(!r.err, "set ap");
        uint32_t cs = htole32(0x2b0b);                /* channel 11 */
        run(&f, req, mkvar(req, true, "chanspec", &cs, 4, 0), &r); CHECK(!r.err, "chanspec");
        v = htole32(1); run(&f, req, mkreq(req, WLC_UP, true, &v, 4, 4), &r); CHECK(!r.err, "up");
        uint8_t pmk[4 + 64] = { 32, 0, 0, 0 };           /* key_len=32, flags=0 (raw PMK) */
        for (int i = 0; i < 32; i++) pmk[4 + i] = (uint8_t)(0xA0 + i);
        run(&f, req, mkreq(req, WLC_SET_WSEC_PMK, true, pmk, sizeof pmk, sizeof pmk), &r); CHECK(!r.err, "psk offload");
        v = htole32(0x80); run(&f, req, mkvar(req, true, "wpa_auth", &v, 4, 0), &r);
        v = htole32(4); run(&f, req, mkreq(req, WLC_SET_WSEC, true, &v, 4, 4), &r);
        v = htole32(0); run(&f, req, mkreq(req, WLC_SET_AUTH, true, &v, 4, 4), &r);
        uint8_t sae[2 + 8] = { 8, 0, 'p', 'a', 's', 's', 'w', 'o', 'r', 'd' };
        run(&f, req, mkvar(req, true, "sae_password", sae, sizeof sae, 0), &r);
        CHECK(r.err && r.status == BCME_UNSUPPORTED, "SAE hotspot refused");

        int joins0 = rec.joins;
        uint8_t ssid[36 + 12] = { 7, 0, 0, 0, 'h', 'o', 't', 's', 'p', 'o', 't' };
        run(&f, req, mkreq(req, WLC_SET_SSID, true, ssid, sizeof ssid, sizeof ssid), &r);
        CHECK(!r.err && ap.starts == 1 && rec.joins == joins0, "SET_SSID in AP mode starts a hotspot, not a join");
        CHECK(ap.req.ssid_len == 7 && !memcmp(ap.req.ssid, "hotspot", 7) && ap.req.channel == 11, "ssid and channel");
        CHECK(ap.req.wpa2_psk && ap.req.have_pmk && ap.req.pmk[0] == 0xA0 && ap.req.pmk[31] == 0xBF, "PMK handed over");
        run(&f, req, mkvar(req, false, "chanspec", NULL, 0, 4), &r);
        CHECK(!r.err && rd32(r.b + 16) == 0x2b0b, "chanspec reports the hotspot channel");

        const uint8_t sta[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
        nhost = 0;
        fw_ap_station(&f, sta, true);
        CHECK(nhost == 1 && ev_code(0) == EVT_ASSOC_IND && ev_status(0) == EVTST_SUCCESS, "ASSOC_IND for a new station");
        /* the event data are the association request elements: SSID, rates, and for WPA2 an RSN element */
        size_t off = 4 + 14 + 10 + 48;
        CHECK(host[0].d[off] == 0 && host[0].d[off + 1] == 7 && !memcmp(host[0].d + off + 2, "hotspot", 7), "IEs start with the SSID");
        CHECK(memmem(host[0].d + off, host[0].n - off, "\x30\x14\x01\x00\x00\x0f\xac\x04", 8) != NULL, "IEs hold an RSN element (CCMP)");
        run(&f, req, mkreq(req, WLC_GET_ASSOCLIST, false, NULL, 0, 64), &r);
        CHECK(!r.err && rd32(r.b + 16) == 1 && !memcmp(r.b + 20, sta, 6), "associated station list");
        run(&f, req, mkvar(req, false, "sta_info", sta, 6, 160), &r);
        CHECK(!r.err && r.b[16] == 4 && (rd32(r.b + 16 + 8) & 0x10), "sta_info: version 4, associated");

        uint8_t scb[10] = { 3, 0, 0, 0 };
        memcpy(scb + 4, sta, 6);
        run(&f, req, mkreq(req, WLC_SCB_DEAUTHENTICATE_FOR_REASON, true, scb, sizeof scb, sizeof scb), &r);
        CHECK(!r.err && ap.deauths == 1 && ap.reason == 3 && !memcmp(ap.sta, sta, 6), "deauthenticate reaches the backend");
        nhost = 0;
        fw_ap_station(&f, sta, false);
        CHECK(nhost == 1 && ev_code(0) == EVT_DISASSOC_IND, "DISASSOC_IND when the station leaves");

        uint8_t empty[36 + 12] = { 0 };
        run(&f, req, mkreq(req, WLC_SET_SSID, true, empty, sizeof empty, sizeof empty), &r);
        CHECK(!r.err && ap.stops == 1, "empty SET_SSID stops the hotspot");
        fw_host_enable(&f, true);
        run(&f, req, mkreq(req, WLC_SET_SSID, true, ssid, sizeof ssid, sizeof ssid), &r);
        CHECK(ap.starts == 2, "hotspot started again");
        fw_host_enable(&f, false);
        CHECK(ap.stops == 2, "the USB host going away stops the hotspot");

        /* open hotspot, and a mode the chip cannot offer */
        fw_reset(&f);
        fw_host_enable(&f, true);
        v = htole32(1); run(&f, req, mkreq(req, WLC_SET_AP, true, &v, 4, 4), &r);
        run(&f, req, mkreq(req, WLC_SET_SSID, true, ssid, sizeof ssid, sizeof ssid), &r);
        CHECK(!r.err && ap.starts == 3 && !ap.req.wpa2_psk, "open hotspot");
        fw_host_enable(&f, false);
        fw_reset(&f);
        v = htole32(1); run(&f, req, mkreq(req, WLC_SET_AP, true, &v, 4, 4), &r);
        v = htole32(0x40000); run(&f, req, mkvar(req, true, "wpa_auth", &v, 4, 0), &r);
        v = htole32(4); run(&f, req, mkreq(req, WLC_SET_WSEC, true, &v, 4, 4), &r);
        run(&f, req, mkreq(req, WLC_SET_SSID, true, ssid, sizeof ssid, sizeof ssid), &r);
        CHECK(r.err && r.status == BCME_UNSUPPORTED && ap.starts == 3, "WPA3 hotspot refused");

        /* what NetworkManager asks for: WPA2-PSK plus PSK-SHA256, management frame protection optional */
        fw_reset(&f);
        fw_host_enable(&f, true);
        v = htole32(1); run(&f, req, mkreq(req, WLC_SET_AP, true, &v, 4, 4), &r);
        run(&f, req, mkreq(req, WLC_SET_WSEC_PMK, true, pmk, sizeof pmk, sizeof pmk), &r);
        v = htole32(0x8080); run(&f, req, mkvar(req, true, "wpa_auth", &v, 4, 0), &r);
        v = htole32(1); run(&f, req, mkvar(req, true, "mfp", &v, 4, 0), &r);
        v = htole32(4); run(&f, req, mkreq(req, WLC_SET_WSEC, true, &v, 4, 4), &r);
        run(&f, req, mkreq(req, WLC_SET_SSID, true, ssid, sizeof ssid, sizeof ssid), &r);
        CHECK(!r.err && ap.starts == 4 && ap.req.psk_sha256 && ap.req.pmf == 1, "WPA2-PSK + SHA256 + optional PMF accepted");
    }

    TEST_DONE("fw");
}
