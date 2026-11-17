#include "wpa_parse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t wpa_hex_encode(const void *in, size_t n, char *out, size_t cap)
{
    static const char d[] = "0123456789abcdef";
    const uint8_t *p = in;
    size_t o = 0;
    for (size_t i = 0; i < n && o + 2 < cap; i++) {
        out[o++] = d[p[i] >> 4];
        out[o++] = d[p[i] & 15];
    }
    if (cap)
        out[o] = '\0';
    return o;
}

static int nib(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int wpa_hex_decode(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (hex[0] && hex[1] && !isspace((unsigned char)hex[0])) {
        int a = nib(hex[0]), b = nib(hex[1]);
        if (a < 0 || b < 0 || n >= cap)
            return -1;
        out[n++] = (uint8_t)(a << 4 | b);
        hex += 2;
    }
    return (int)n;
}

int wpa_freq_to_chan(int mhz)
{
    if (mhz >= 2412 && mhz <= 2472 && (mhz - 2407) % 5 == 0)
        return (mhz - 2407) / 5;
    if (mhz == 2484)
        return 14;
    return 0;
}

int wpa_chan_to_freq(int chan)
{
    if (chan >= 1 && chan <= 13)
        return 2407 + 5 * chan;
    return chan == 14 ? 2484 : 0;
}

const char *wpa_kv(const char *text, const char *key, size_t *len)
{
    size_t kl = strlen(key);
    const char *p = text;
    while (*p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            const char *e = strchr(v, '\n');
            if (len)
                *len = e ? (size_t)(e - v) : strlen(v);
            return v;
        }
        p = strchr(p, '\n');
        if (!p)
            break;
        p++;
    }
    return NULL;
}

static long kv_long(const char *text, const char *key, long dflt, int base)
{
    const char *v = wpa_kv(text, key, NULL);
    return v ? strtol(v, NULL, base) : dflt;
}

bool wpa_parse_bss(const char *text, struct wl_bss *b, int *age_s)
{
    memset(b, 0, sizeof *b);
    size_t l;
    const char *bssid = wpa_kv(text, "bssid", &l);
    unsigned m[6];
    if (!bssid || sscanf(bssid, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++)
        b->bssid[i] = (uint8_t)m[i];

    int chan = wpa_freq_to_chan((int)kv_long(text, "freq", 0, 10));
    if (!chan)
        return false;                                        /* 5 GHz etc: the Pi's radio is 2.4 GHz only */
    b->channel = (uint8_t)chan;
    b->beacon_int = (uint16_t)kv_long(text, "beacon_int", 100, 10);
    b->capability = (uint16_t)kv_long(text, "capabilities", 0, 16);
    long lvl = kv_long(text, "level", -90, 10);
    b->rssi = (int8_t)(lvl < -127 ? -127 : lvl > 0 ? 0 : lvl);
    if (age_s)
        *age_s = (int)kv_long(text, "age", 0, 10);

    /* The `ie` field is the probe response (or beacon) information elements: exactly what the
     * firmware reports. Fall back to the beacon's when the probe response was not seen. */
    const char *ie = wpa_kv(text, "ie", &l);
    if (!ie || l < 4)
        ie = wpa_kv(text, "beacon_ie", &l);
    if (ie && l >= 4) {
        char tmp[2 * BSS_MAX_IE + 8];
        size_t n = l < sizeof tmp - 1 ? l : sizeof tmp - 1;
        memcpy(tmp, ie, n);
        tmp[n] = '\0';
        int got = wpa_hex_decode(tmp, b->ie, sizeof b->ie);
        if (got > 0) {
            /* keep whole elements only */
            size_t o = 0;
            while (o + 2 <= (size_t)got && o + 2 + b->ie[o + 1] <= (size_t)got)
                o += 2u + b->ie[o + 1];
            b->ie_len = (uint16_t)o;
        }
    }

    /* SSID: from the IEs (id 0) so non-printable bytes survive; else the textual field. */
    for (size_t o = 0; o + 2 <= b->ie_len; o += 2u + b->ie[o + 1]) {
        if (b->ie[o] == 0 && b->ie[o + 1] <= 32) {
            b->ssid_len = b->ie[o + 1];
            memcpy(b->ssid, b->ie + o + 2, b->ssid_len);
            break;
        }
    }
    return true;
}

int wpa_event_int(const char *msg, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = msg; (p = strstr(p, key)); p++)
        if (p[kl] == '=' && (p == msg || p[-1] == ' '))
            return atoi(p + kl + 1);
    return -1;
}

void wpa_build_scan(const struct wl_scan_req *req, bool with_directed, int directed_idx, char *out, size_t cap)
{
    size_t o = (size_t)snprintf(out, cap, "SCAN");
    if (req->n_channels) {
        o += (size_t)snprintf(out + o, cap - o, " freq=");
        for (int i = 0; i < req->n_channels && o + 8 < cap; i++)
            o += (size_t)snprintf(out + o, cap - o, "%s%d", i ? "," : "", wpa_chan_to_freq(req->channels[i]));
    }
    if (with_directed && directed_idx >= 0 && directed_idx < req->n_ssids && req->ssids[directed_idx].len) {
        char hex[80];
        wpa_hex_encode(req->ssids[directed_idx].ssid, req->ssids[directed_idx].len, hex, sizeof hex);
        o += (size_t)snprintf(out + o, cap - o, " ssid %s", hex);
    }
}

static void add(struct wpa_net *n, const char *key, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#include <stdarg.h>
static void add(struct wpa_net *n, const char *key, const char *fmt, ...)
{
    if (n->n >= WPA_MAX_NET_OPTS)
        return;
    n->opt[n->n].key = key;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(n->opt[n->n].value, sizeof n->opt[n->n].value, fmt, ap);
    va_end(ap);
    n->n++;
}

void wpa_net_from_join(const struct wl_join_req *j, struct wpa_net *net)
{
    memset(net, 0, sizeof *net);
    char hex[80];
    wpa_hex_encode(j->ssid, j->ssid_len, hex, sizeof hex);
    add(net, "ssid", "%s", hex);
    add(net, "scan_ssid", "1");                                 /* also finds hidden networks */

    const uint32_t a = j->wpa_auth;
    const bool wpa1 = a & (WPA_AUTH_UNSPECIFIED | WPA_AUTH_PSK);
    const bool wpa2 = a & (WPA2_AUTH_UNSPECIFIED | WPA2_AUTH_PSK | WPA2_AUTH_1X_SHA256 | WPA2_AUTH_PSK_SHA256);
    const bool sae  = a & WPA3_AUTH_SAE_PSK;
    const bool psk  = a & (WPA_AUTH_PSK | WPA2_AUTH_PSK | WPA2_AUTH_PSK_SHA256);
    const bool eap  = a & (WPA_AUTH_UNSPECIFIED | WPA2_AUTH_UNSPECIFIED | WPA2_AUTH_1X_SHA256);

    if (eap && !psk && !sae) {
        net->unsupported = true;
        net->why = "802.1X (WPA-Enterprise) cannot be offloaded to this dongle";
        return;
    }
    if (!wpa1 && !wpa2 && !sae) {
        if (j->wsec & 1) {                                      /* WEP */
            net->unsupported = true;
            net->why = "WEP is not supported";
            return;
        }
        add(net, "key_mgmt", "NONE");
        return;
    }

    /* cipher bits: 2 = TKIP, 4 = AES (CCMP) */
    const char *cipher = (j->wsec & 2) && (j->wsec & 4) ? "CCMP TKIP" : (j->wsec & 2) ? "TKIP" : "CCMP";
    add(net, "pairwise", "%s", cipher);
    add(net, "group", "%s", cipher[0] == 'T' ? "TKIP" : "CCMP TKIP");

    if (sae && !psk) {
        add(net, "key_mgmt", "SAE");
        add(net, "proto", "RSN");
        add(net, "ieee80211w", "2");
        if (j->have_sae)
            add(net, "sae_password", "\"%s\"", j->sae_password);
        else if (j->have_passphrase)
            add(net, "sae_password", "\"%s\"", j->passphrase);
        else {
            net->unsupported = true;
            net->why = "WPA3-SAE needs the password, which the host did not provide";
        }
        return;
    }

    /* WPA/WPA2-PSK (possibly WPA3 transition mode) */
    char km[64] = "WPA-PSK";
    if (a & WPA2_AUTH_PSK_SHA256)
        strcat(km, " WPA-PSK-SHA256");
    if (sae)
        strcat(km, " SAE");
    add(net, "key_mgmt", "%s", km);
    add(net, "proto", "%s", wpa1 && wpa2 ? "WPA RSN" : wpa1 ? "WPA" : "RSN");
    add(net, "ieee80211w", "%u", sae ? (j->mfp ? j->mfp : 1) : (j->mfp > 2 ? 2 : j->mfp));
    if (j->have_pmk) {
        char p[80];
        wpa_hex_encode(j->pmk, 32, p, sizeof p);
        add(net, "psk", "%s", p);                               /* 64 hex digits = a ready-made PMK */
    } else if (j->have_passphrase) {
        add(net, "psk", "\"%s\"", j->passphrase);
    } else if (j->have_sae) {
        add(net, "sae_password", "\"%s\"", j->sae_password);
    } else {
        net->unsupported = true;
        net->why = "no PSK material was provided by the host";
    }
    if (sae && j->have_sae)
        add(net, "sae_password", "\"%s\"", j->sae_password);
}
