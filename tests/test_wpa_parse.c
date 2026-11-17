#include "check.h"
#include "wpa_parse.h"

static const char *BSS_REPLY =
    "id=0\nbssid=02:11:22:33:44:55\nfreq=2442\nbeacon_int=200\ncapabilities=0x1c11\nqual=0\nnoise=-89\n"
    "level=-44\ntsf=0000000000000000\nage=5\n"
    "ie=0007486f6d654e6574010882848b961224486c030107\n"
    "flags=[WPA2-PSK-CCMP][ESS]\nssid=HomeNet\nsnr=45\n";

int main(void)
{
    uint8_t b[8];
    CHECK(wpa_hex_decode("00ff10", b, sizeof b) == 3 && b[0] == 0 && b[1] == 0xff && b[2] == 0x10, "hex decode");
    CHECK(wpa_hex_decode("0g", b, sizeof b) == -1, "bad hex rejected");
    CHECK(wpa_hex_decode("0011223344556677", b, 4) == -1, "overflow rejected");
    char h[16];
    wpa_hex_encode("\x01\xab", 2, h, sizeof h);
    CHECK(!strcmp(h, "01ab"), "hex encode");

    CHECK(wpa_freq_to_chan(2412) == 1 && wpa_freq_to_chan(2472) == 13 && wpa_freq_to_chan(2484) == 14, "freq->chan");
    CHECK(wpa_freq_to_chan(5180) == 0 && wpa_freq_to_chan(2413) == 0, "5 GHz / odd freq rejected");
    CHECK(wpa_chan_to_freq(6) == 2437 && wpa_chan_to_freq(14) == 2484 && wpa_chan_to_freq(15) == 0, "chan->freq");

    /* BSS reply */
    struct wl_bss bss;
    int age = -1;
    CHECK(wpa_parse_bss(BSS_REPLY, &bss, &age), "parse bss");
    CHECK(bss.bssid[0] == 0x02 && bss.bssid[5] == 0x55, "bssid");
    CHECK(bss.channel == 7 && bss.beacon_int == 200 && bss.capability == 0x1c11 && bss.rssi == -44 && age == 5,
          "fields: ch=%u bi=%u cap=%#x rssi=%d age=%d", bss.channel, bss.beacon_int, bss.capability, bss.rssi, age);
    CHECK(bss.ssid_len == 7 && !memcmp(bss.ssid, "HomeNet", 7), "ssid taken from the IE");
    CHECK(bss.ie_len == 2 + 7 + 2 + 8 + 3 && bss.ie[0] == 0, "ies kept whole (%u)", bss.ie_len);

    /* a truncated element at the end is cut off, not half-copied */
    const char *trunc = "bssid=01:02:03:04:05:06\nfreq=2412\nie=0005414243\nssid=x\n";
    CHECK(wpa_parse_bss(trunc, &bss, NULL) && bss.ie_len == 0, "incomplete trailing IE dropped");
    CHECK(!wpa_parse_bss("bssid=01:02:03:04:05:06\nfreq=5180\nie=00\n", &bss, NULL), "5 GHz BSS rejected");
    CHECK(!wpa_parse_bss("", &bss, NULL), "empty reply rejected");

    CHECK(wpa_event_int("<3>CTRL-EVENT-DISCONNECTED bssid=aa:bb reason=3 locally_generated=1", "reason") == 3, "event int");
    CHECK(wpa_event_int("CTRL-EVENT-SSID-TEMP-DISABLED id=0 auth_failures=1", "auth_failures") == 1, "event int 2");
    CHECK(wpa_event_int("xreason=7", "reason") == -1, "key must start a token");

    /* scan command */
    struct wl_scan_req sr = { .n_ssids = 2, .n_channels = 2 };
    sr.channels[0] = 1; sr.channels[1] = 6;
    sr.ssids[1].len = 4; memcpy(sr.ssids[1].ssid, "home", 4);
    char cmd[200];
    wpa_build_scan(&sr, false, 0, cmd, sizeof cmd);
    CHECK(!strcmp(cmd, "SCAN freq=2412,2437"), "broadcast scan: '%s'", cmd);
    wpa_build_scan(&sr, true, 1, cmd, sizeof cmd);
    CHECK(!strcmp(cmd, "SCAN freq=2412,2437 ssid 686f6d65"), "directed scan: '%s'", cmd);
    struct wl_scan_req all = { 0 };
    wpa_build_scan(&all, false, 0, cmd, sizeof cmd);
    CHECK(!strcmp(cmd, "SCAN"), "all channels: '%s'", cmd);

    /* ---- network mapping */
    struct wpa_net n;
    struct wl_join_req j;
    /* a missing option yields "(none)" so a failed lookup is a test failure, not a crash */
    #define OPT(name) ({ const char *r = "(none)"; for (int i = 0; i < n.n; i++) if (!strcmp(n.opt[i].key, name)) r = n.opt[i].value; r; })

    memset(&j, 0, sizeof j);
    j.ssid_len = 4; memcpy(j.ssid, "home", 4);
    wpa_net_from_join(&j, &n);
    CHECK(!n.unsupported && !strcmp(OPT("key_mgmt"), "NONE") && !strcmp(OPT("ssid"), "686f6d65"), "open network");

    j.wpa_auth = WPA2_AUTH_PSK; j.wsec = 4; j.have_pmk = true;
    for (int i = 0; i < 32; i++) j.pmk[i] = (uint8_t)i;
    wpa_net_from_join(&j, &n);
    CHECK(!n.unsupported && !strcmp(OPT("key_mgmt"), "WPA-PSK") && !strcmp(OPT("proto"), "RSN"), "WPA2-PSK");
    CHECK(!strcmp(OPT("psk"), "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"),
          "PMK becomes a 64-digit hex psk (no quotes)");
    CHECK(!strcmp(OPT("pairwise"), "CCMP") && !strcmp(OPT("ieee80211w"), "0"), "ccmp, no pmf");

    j.wpa_auth = WPA_AUTH_PSK | WPA2_AUTH_PSK; j.wsec = 6;
    wpa_net_from_join(&j, &n);
    CHECK(!strcmp(OPT("proto"), "WPA RSN") && !strcmp(OPT("pairwise"), "CCMP TKIP"), "mixed WPA/WPA2, TKIP+CCMP");

    j.wpa_auth = WPA2_AUTH_PSK; j.wsec = 4; j.have_pmk = false; j.have_passphrase = true;
    strcpy(j.passphrase, "pass word");
    wpa_net_from_join(&j, &n);
    CHECK(!strcmp(OPT("psk"), "\"pass word\""), "passphrase is quoted");

    j.have_passphrase = false;
    wpa_net_from_join(&j, &n);
    CHECK(n.unsupported, "PSK network without key material is refused");

    memset(&j, 0, sizeof j);
    j.ssid_len = 1; j.ssid[0] = 'x';
    j.wpa_auth = WPA3_AUTH_SAE_PSK; j.wsec = 4; j.have_sae = true; strcpy(j.sae_password, "hunter2");
    wpa_net_from_join(&j, &n);
    CHECK(!n.unsupported && !strcmp(OPT("key_mgmt"), "SAE") && !strcmp(OPT("ieee80211w"), "2") &&
          !strcmp(OPT("sae_password"), "\"hunter2\""), "WPA3-SAE");

    j.wpa_auth = WPA2_AUTH_PSK | WPA3_AUTH_SAE_PSK; j.have_pmk = true; j.mfp = 1;
    wpa_net_from_join(&j, &n);
    CHECK(!strcmp(OPT("key_mgmt"), "WPA-PSK SAE") && !strcmp(OPT("ieee80211w"), "1"), "WPA2/WPA3 transition");

    memset(&j, 0, sizeof j);
    j.ssid_len = 1; j.ssid[0] = 'x'; j.wpa_auth = WPA2_AUTH_UNSPECIFIED;
    wpa_net_from_join(&j, &n);
    CHECK(n.unsupported && n.why, "WPA-Enterprise is refused with a reason");
    memset(&j, 0, sizeof j);
    j.ssid_len = 1; j.ssid[0] = 'x'; j.wsec = 1;
    wpa_net_from_join(&j, &n);
    CHECK(n.unsupported, "WEP is refused");

    TEST_DONE("wpa_parse");
}
