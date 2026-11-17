# pi-radio-gadget: how it works

This document explains the design and, more importantly, the *why*: most decisions were forced by how Linux drivers on the PC and
the Pi's USB controller actually behave. Practical notes on the quirks are collected in [DEVELOPMENT.md](DEVELOPMENT.md).

```
                         USB (high speed)
   PC                                                  Raspberry Pi Zero 2 W
 ┌────────────────────┐        ┌────────────────────────────────────────────────────────┐
 │ NetworkManager     │        │  dwc2 UDC ── configfs composite gadget                  │
 │ wpa_supplicant /   │        │     ├─ ffs.wlan (interface 0: 2 bulk endpoints)         │
 │   hostapd          │ <────> │     ├─ ffs.hci  (interface 1: interrupt + 2 bulk)       │
 │ brcmfmac (stock)   │        │     └─ ffs.sco  (interface 2: isochronous audio)        │
 │ btusb + bluetoothd │        │            │                │                           │
 │ PipeWire (calls)   │        │      pi-radio-gadget daemon (one process)                │
 └────────────────────┘        │      ┌─────────┴──────┐   ┌──┴────────────┐             │
                               │      │ wlan function  │   │ hci + sco     │             │
                               │      │  fw.c emulated │   │ HCI relay,    │             │
                               │      │  Broadcom FW   │   │ call audio    │             │
                               │      └───┬────────────┘   └──┬────────────┘             │
                               │   backend│(sup)              │ HCI user channel          │
                               │  wpa_supplicant / hostapd    BT chip (BCM43436)         │
                               │    + AF_PACKET tap                                      │
                               │   WiFi chip (brcmfmac FullMAC, 2.4 GHz)                 │
                               └────────────────────────────────────────────────────────┘
```

## 1. Why a Broadcom FullMAC dongle?

The PC must be able to "connect to any WiFi like a normal dongle". Three ways to present a WiFi adapter to a PC were considered:

| Approach | Verdict |
|---|---|
| USB Ethernet (NCM/ECM) with NAT on the Pi | Works, but the PC sees a wired NIC: no scanning, no SSID selection, nothing a WiFi dongle does. Rejected: not what was asked. |
| Emulate a SoftMAC dongle (the earlier prototype emulated a MediaTek MT7601U) | The PC then does 802.11 itself and the Pi would have to inject/receive raw frames and emulate the chip's hardware key tables, TX status, calibration, and so on. The Pi's radio (a FullMAC chip) cannot do that. The prototype also needed 8 USB endpoints; the Pi's dwc2 has 7. |
| **Emulate a Broadcom FullMAC USB dongle** | **Chosen.** The PC's `brcmfmac` driver delegates scanning, association and WPA to "firmware" through a high-level command protocol. The Pi's own chip *is* a Broadcom FullMAC part, so almost every command maps onto what `wpa_supplicant` or `hostapd` can ask the Pi's radio. |

Three properties of `brcmfmac` make this practical:

1. **No firmware download.** If the device answers the bootloader request `DL_GETVER` with chip id `0xA123` ("post-boot"), the driver
   assumes the firmware is already running and skips the download. The PC needs no firmware file, and the official USB id
   `0a5c:0bdc` ("BCM dongle, firmware loaded") is used.
2. **Only two bulk endpoints** are required (control traffic uses ep0).
3. **Works in a composite device.** `brcmfmac` checks the *device* class (FF, EF or E0), a single configuration, and an interface of class
   FF/02/FF; `btusb` matches interfaces of class E0/01/01. The interface *numbers* are free, with the caveat below.

## 2. The composite gadget and why the order matters

`pi-radio-gadget-setup.sh` creates one gadget (configfs) with three FunctionFS functions. The **order of the symlinks is load-bearing**:

* `ffs.wlan` must be first, so it becomes USB **interface 0**. `brcmfmac` sends its probe-time bootloader requests (`DL_GETVER`) with
  `wIndex = 0` *before* it has recorded its interface number; the kernel routes interface-recipient requests by `wIndex`, so
  interface 0 has to be the WiFi function.
* `ffs.hci` second, and it alone sets `FUNCTIONFS_ALL_CTRL_RECIP`. `btusb` sends HCI commands as a class request addressed to the
  *device* (`bmRequestType 0x20`, `wIndex 0`). The composite core hands device-recipient requests to the first function whose
  `req_match` accepts them, and a FunctionFS function accepts them only with that flag. With the flag only on the Bluetooth
  function, WiFi requests (interface recipient) reach WiFi and HCI commands reach Bluetooth.
* `ffs.sco` third: the call-audio interface, USB interface 2, directly after the HCI one as `btusb` expects. It is a function of its
  own because FunctionFS re-enables *every* endpoint of a function whenever the host selects an alternate setting. With the audio
  interface inside `ffs.hci`, each call start and stop reset the Bluetooth control and ACL endpoints under the host, and calls then
  often died within tens of seconds with a stuck bulk-IN transfer (section 5). Apart, a 150 s call ran clean.

Device class is `EF/02/01` so both drivers match by interface. The functions use 5 IN endpoints (WiFi bulk, Bluetooth interrupt and
bulk, two isochronous audio) and 4 OUT endpoints (WiFi bulk, Bluetooth bulk, two isochronous audio) on top of ep0.

## 3. The WiFi function

### Control path (ep0)
| Request | Meaning |
|---|---|
| vendor IN `DL_GETVER` (5) | bootloader id; answered `0xA123` = firmware already running. Also resets the emulated firmware state: a new driver probe starts clean. |
| vendor IN `DL_GETSTATE`/`DL_RESETCFG` | acknowledged |
| class OUT, bRequest 0 | `SEND_ENCAPSULATED_COMMAND`: a **dcmd**, the host's ioctl/iovar request |
| class IN, bRequest 1 | `GET_ENCAPSULATED_RESPONSE`: our answer to the previous dcmd |

A dcmd is `{cmd, len, flags(id, set, ifidx), status}` followed by `len` bytes. `cmd` is either an ioctl number or `GET_VAR`/`SET_VAR`
carrying `"name\0" + data` (an *iovar*). The response echoes the request id (the driver retries on a mismatch) and sets the error flag
and a negative status for unsupported requests.

### Data path (bulk)
Every packet carries a 4-byte **BDC** header, then the Ethernet frame. Firmware **events** are also Ethernet frames (ethertype `0x886c`)
with a Broadcom event header; the driver recognizes them by type and OUI. Scan results, join results, link changes and hotspot
station arrivals are all events.

### What the emulated firmware (`src/fw.c`) answers
The driver's feature detection decides what it will ask for, so `fw.c` advertises only what can be honoured:

* **Present:** PSK offload (`sup_wpa`), PMF (`mfp`), D11N chanspecs, escan, 2.4 GHz channels 1-13, AP PSK offload.
  `cap` = `sta idauth` (plus `sae` when enabled).
* **Absent on purpose (answered `BCME_UNSUPPORTED`):** p2p, mbss, mchan, tdls, pfn/gscan, wowl, rsdb, 802.11ac, 40 MHz, `scan_ver` (so
  the driver uses the simple v1 scan format).
* **Details learned from the driver source:**
  * `chanspecs` takes an optional *filter* chanspec; the driver probes 40 MHz with it and `WARN_ON`s if the answer is not 40 MHz
    entries, so a 20 MHz-only device must answer "none".
  * `per_chan_info` must *succeed*: `brcmf_construct_chaninfo` returns the error of the **last** query as its result.
  * Broadcom numbers get/set ioctls as pairs (`GET_PM`=85 / `SET_PM`=86); the generic "remember what was set" logic maps a get to its
    set partner.
  * `event_msgs` is a bit mask; events are delivered only if the driver unmasked them.

### Association (station mode)
The PC's wpa_supplicant asks cfg80211 to connect; `brcmfmac` programs `wpa_auth`, `wsec`, `mfp`, passes the **PMK** it derived
(`WLC_SET_WSEC_PMK`) or the **SAE password**, then sends the `join` iovar. The backend turns those into a private `wpa_supplicant`
network block on the Pi (`src/wpa_parse.c`, unit tested): the PMK becomes `psk=<64 hex digits>`, which wpa_supplicant treats as a
ready-made PMK. The Pi therefore never needs the passphrase, and the Pi's FullMAC chip performs the 4-way handshake itself, exactly
as a real dongle would. On success the firmware emits `PSK_SUP(completed)`, `SET_SSID(success)`, `LINK(up)`: the three events the
driver needs.

Not supported (refused cleanly, with the reason logged): WPA-Enterprise (the EAP exchange would have to be relayed), WEP.

### Hotspot (AP mode)
The firmware advertises AP PSK offload (`idauth`), so the PC's hostapd/wpa_supplicant hands over SSID, channel and the 32-byte PSK and
expects the firmware to run the whole access point:

1. The driver's start sequence (`WLC_SET_AP`, `chanspec`, `WLC_SET_WSEC_PMK`, `wpa_auth`/`wsec`/`mfp`, `WLC_SET_SSID`, `closednet`) is
   collected by `fw.c` and, at `WLC_SET_SSID`, turned into a `wl_ap_req` for the backend (`ap_set_ssid`). An empty `WLC_SET_SSID`
   stops the hotspot. Only open and WPA2-PSK/CCMP (optionally with PSK-SHA256 and PMF) are accepted; SAE is refused because the
   chip cannot do it, and so are WPA, WEP and 802.1X.
2. The Pi backend stops its wpa_supplicant (the interface changes owner), writes a hostapd configuration (`ssid2=<hex>`,
   `wpa_psk=<hex>`, `wpa_key_mgmt=WPA-PSK [WPA-PSK-SHA256]`, `ieee80211w`) and runs hostapd on the same interface, then polls its
   control socket until it reports `state=ENABLED`. hostapd's output goes to the boot partition for diagnosis.
3. The AF_PACKET tap carries the stations' Ethernet frames both ways, exactly as in station mode: DHCP and everything else is
   handled by the PC (for NetworkManager's shared mode: its dnsmasq, with NAT out through the PC's own uplink).
4. hostapd's `AP-STA-CONNECTED`/`AP-STA-DISCONNECTED` become `ASSOC_IND`/`DISASSOC_IND` events. The `ASSOC_IND` data are
   association-request elements rebuilt from what the hotspot offers (SSID, rates, an RSN element); the driver passes them to cfg80211
   and the PC's hostapd validates them against its configuration.
5. `sta_info` (version 4) reports each station as associated, authorized and never idle, at the offsets of the kernel's unpacked
   struct, so the PC does not age connected stations out. `WLC_SCB_DEAUTHENTICATE_FOR_REASON` kicks a station through hostapd.

While a hotspot runs there is no station connection (one radio, and the supplicant is stopped): scans return empty and joins are
refused. If hostapd dies the firmware returns to idle. The Pi's chip supports one AP next to a station on the same channel, but this
daemon does not use that.

### The radio backend (`src/backend_sup.c`)
* Takes `wlan0` from NetworkManager (`nmcli device set wlan0 managed no`), removes its IP addresses and IPv6 so the Pi itself never
  talks on it, switches radio power saving off (a mostly idle radio otherwise answers late), and runs a private `wpa_supplicant`
  controlled over its control socket.
* **Scan:** `SCAN` then `BSS <n>`; the `ie=` field is exactly the information-element blob the firmware format needs. Hidden networks
  requested by name get a directed scan.
* **Data:** an `AF_PACKET` tap (`src/tap.c`). The Pi's chip converts 802.11 <-> Ethernet, so frames pass through as Ethernet. EAPOL is
  dropped both ways (the handshake belongs to the Pi), the Pi's own transmissions are not looped back, and a foreign source MAC is
  rewritten to the station's (a FullMAC station can only speak as itself).
* **MAC address:** NetworkManager randomizes the MAC; the driver forwards the change (`cur_etheraddr`), and the backend applies it to
  `wlan0` (restarting its supplicant around the interface bounce) and restores the factory address on exit.
* **Regulatory domain:** the domain the PC's driver selects is passed on to the Pi's radio (`SET country`), unless it is the world domain.
* **Roaming:** a disconnect followed by a reconnect within 3 s is treated as a roam and hidden from the PC; otherwise the PC gets a
  link-down event.
* **Hand-back:** on exit (and from `pi-radio-gadget-radio.sh` if the daemon dies) the interface is returned to NetworkManager.

## 4. The Bluetooth function

### HCI relay
HCI commands (ep0) and ACL (bulk OUT) go to the controller through an **HCI user channel** socket; events (interrupt IN) and ACL (bulk
IN) come back. Nothing is interpreted, except what call audio needs (below). Details worth knowing:

* USB carries no H4 type byte (the endpoint implies it); the socket does, so it is added and stripped.
* An exact multiple of the endpoint packet size needs a trailing zero-length packet, or the host cannot tell the transfer ended (a
  16-byte event on the 16-byte interrupt endpoint).
* Bulk OUT reads may deliver partial or concatenated ACL packets; `acl_split.c` re-frames them and treats an implausible length as
  corruption (drop and resync) instead of waiting forever.
* While the USB host is not enabled, controller traffic is dropped, not queued: stale events from before a reconnect confuse the host.
* **The Pi's UART Bluetooth controller does not survive a close/reopen cycle**: after the daemon exits, the controller is mute until
  `hci_uart` is reloaded (which re-runs the firmware patch and baud setup). `pi-radio-gadget-setup.sh` reloads it on every start.

### Call audio (SCO)
* **Alternate settings.** The host driver switches the isochronous interface between alternate settings. FunctionFS never reports
  which one, and rejects the same endpoint in two settings, so `ffs.sco` declares setting 0 (none), 1 (transparent/mSBC) and 2 (CVSD,
  16-bit voice), each with its own isochronous endpoint pair. The live setting is derived from the commands the host sends:
  Write_Voice_Setting and Setup/Accept_Synchronous_Connection (`sco.c`).
* **Framing.** Frames are 63 bytes: one SCO packet (3-byte header + 60 bytes) per 1 ms USB frame. `btusb` discards a whole packet if any
  of its frames is missing, so smaller frames lost about half the audio.
* **Routing.** The Pi's chip sends call audio to its PCM pins by default. With `--sco-over-hci` a Broadcom vendor command (`0xFC1C`)
  routes it over HCI, re-sent after every HCI reset (its answer is swallowed).
* **Towards the host** the USB controller (dwc2) gives each queued isochronous request its own frame and fails a late one with
  `ENODATA`, so `sco_pace_writer` submits one request per millisecond through AIO, empty ones when there is no audio.
* **From the host** a read on the isochronous OUT endpoint must not be longer than `wMaxPacketSize` (dwc2 returns `EINVAL`), and an
  `ENODATA` (empty frame) must be retried at once; the packets are re-assembled and sent to the chip.
* `pi-radio-gadget-flash --status` shows live counters (`sco[...] rd[...] acl[...]`).

## 5. The USB stall: an open problem, measured, mitigated and self-healing

The Pi Zero 2 W's `dwc2` device controller (internal DMA mode, which cannot be turned off from the device tree) shows an
**intermittent stuck bulk-IN transfer** with this userspace FunctionFS gadget. A queued IN write simply never completes: the host stops
polling that endpoint, everything queued behind the write stalls, and the PC reports `brcmf_escan_timeout`. The earlier prototype hit
the same symptom. The root cause was **not proven**. What was established by experiment (Pi Zero 2 W and a Linux PC, kernel 6.18):

* **The device is healthy and the host is not asking.** The controller dump taken during a stall (it is in the daemon log at the moment
  of the reconnect) shows the endpoint's DMA pointer advanced by the length of the stuck packet (the data is already in the TX FIFO),
  the endpoint armed, and no IN token received. So the *host* had stopped polling it. A host endpoint stops being polled when its xHCI
  endpoint is halted after a bus-level transaction error, and `brcmfmac` never clears a halt. This is the best-fitting explanation, **not
  a confirmed one**. A different cable, port or a powered hub is the first thing to try.
* **Not** link speed (high and full speed fail at the same rate), **not** power (`vcgencmd get_throttled` is 0), **not** Bluetooth (it
  happens with the WiFi function alone).
* It depends on how long the endpoint **sat idle** before the write: roughly 1 in 10 writes after more than half a second of quiet,
  1 in 1,000 after 25 ms, 1 in 13,500 after 3 ms, and none in 90,000 writes at 0.5 ms spacing.
* **Call audio made it much more likely until the audio interface got its own function.** Calls with the audio interface inside
  `ffs.hci` stalled the Bluetooth bulk endpoint within roughly 20-45 s in most attempts (FunctionFS reset the Bluetooth endpoints at
  every alternate-setting change while the host kept transfers queued on them). After the split, a 150 s call ran clean, and a
  later 95 s one as well. Longer calls are untested.

What the daemon does about it, in layers:

1. **Keep-alive.** When the WiFi IN pipe has been idle for 2 ms (`--keepalive-ms`, 0 = off) the daemon sends a 76-byte firmware event
   with a code the driver ignores, so a real frame is never the first write after a pause. It costs about 2-3 % of one Pi core.
2. **One process-wide I/O lock** (`ffs_io_lock`): control requests and bulk/interrupt IN writes never run at the same time, and the two
   halves of a dcmd (OUT, then the IN that fetches the answer) are held under one lock acquisition.
3. **Watchdog.** A write pending for more than 100 ms is interrupted (EINTR makes FunctionFS dequeue it), the endpoint's TX FIFO is
   flushed, and the write is re-issued. This heals the stall that happens on the first write after enumeration. The isochronous audio
   writers are exempt (late audio is simply lost).
4. **USB reconnect.** A write still stuck after 2 s (20 s if the host has not read anything since it enabled the device, because its
   driver can take seconds to probe) makes the daemon soft-disconnect and reattach the gadget, after logging a state dump of the
   controller's endpoints. The PC re-enumerates and re-probes within about 3 s. This heals everything seen, at the price of WiFi and
   Bluetooth both disappearing for a few seconds, and an association being lost.

Expect an occasional few-second interruption and treat sustained throughput over hours as unverified. Worth trying if it matters: a
different kernel version or USB controller, or a PC-side look at the xHCI endpoint state during a stall.

## 6. Updating and maintenance over the USB link

The control endpoint of the Bluetooth function also answers vendor requests that manage the daemon itself (`src/update.c`, tool
`tools/flash.c`):

| Request | Direction | Meaning |
|---|---|---|
| `0x50` BEGIN | OUT | length and SHA-256 of a new binary |
| `0x51` DATA | OUT | a chunk (at most 4096 bytes), offset in `wValue`/`wIndex` |
| `0x52` COMMIT | OUT | `"COMT"`: verify length, hash and ELF magic; keep the running binary as `.prev`; replace the target atomically; write `pending`; restart |
| `0x53` STATUS | IN | one text line: version, state, live counters |
| `0x54` STOP | OUT | `"STOP"`: create `/boot/firmware/pi-radio-gadget.disable` and exit (maintenance mode) |

The daemon exits with status 75 after a commit, so systemd (`Restart=on-failure`) starts the new binary. `pi-radio-gadget-update.sh` (run
by `ExecStartPre`) counts the starts while `pending` exists and restores `.prev` on the second failed start; the daemon deletes
`pending` after 60 s of uptime. There is no signature (a lost key would lock the owner out; the trust boundary is physical access to
the USB cable). The same script applies an update folder on the SD card, once.

## 7. Threads

| Thread | Blocks on | Does |
|---|---|---|
| ep0 loop (per function) | `read(ep0)` | USB events and control requests (under the I/O lock) |
| IN writer (per IN endpoint) | its packet queue | queue -> `write(ep)`, with the stuck-write watchdog |
| `sco_pace_writer` (per audio setting) | a 1 ms timer | one isochronous request per frame, through AIO |
| bulk-OUT readers | `read(ep)` | host data -> firmware / HCI socket; audio frames -> chip |
| `chip_reader` (Bluetooth) | `recv(hci)` | controller packets -> queues |
| supervisor (WiFi backend) | control-socket events and a job queue | scan / join / leave / MAC / hotspot jobs, link polling, hostapd health |
| tap receiver | `recvfrom(AF_PACKET)` | radio frames -> host |
| watchdog | timer | kicks stuck IN writes, reconnects the USB link |

Firmware-facing backend operations only enqueue work (the firmware holds its lock while calling them); results come back through the
`fw_*` callbacks from the supervisor thread.

## 8. Testing

`meson test` runs, with no hardware:

* `ffs`: descriptor blobs, byte-for-byte against known-good Bluetooth descriptors and structurally for the audio function
* `acl_split`, `pktq`, `bdc`, `epwriter`: framing, queue and writer logic
* `sco`: alternate-setting selection from captured command bytes, packet chunking and re-assembly
* `fw`: the emulated firmware driven exactly as `brcmfmac` drives it (init sequence, scan, join, events, data path, reset, and the
  complete hotspot start/stop dialogue with a recording backend)
* `wpa_parse`: supplicant reply parsing and the firmware-to-wpa_supplicant network mapping
* `update`: SHA-256 vectors, the upload protocol, refusals, maintenance mode
* `tap`: the data path against a veth pair (root only; skipped otherwise)

The `fake` backend (`--wlan-backend fake`) serves synthetic networks without touching the radio: useful for bring-up and for
stress-testing the USB side while keeping SSH access. What was verified on real hardware is listed in the README.
