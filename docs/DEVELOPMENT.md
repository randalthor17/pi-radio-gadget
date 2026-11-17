# Development

## Build and test

```
nix-shell -p gcc meson ninja          # or: sudo apt install gcc meson ninja-build
meson setup build
meson test -C build                   # unit tests, no hardware needed
```

The `tap` test needs root (a veth pair) and is skipped otherwise. `meson setup build -Dwarning_level=3` is a stricter compile; the
tree builds without warnings at the default level.

Cross-build for the Pi: `scripts/cross-build.sh` (static aarch64 with musl; glibc static linking is fragile, musl is not). Push to a
running Pi: `build/pi-radio-gadget-flash build-aarch64/pi-radio-gadget`.

## Layout

| Path | Contents |
|---|---|
| `src/main.c` | options, startup order (prepare functions, attach the UDC, run, start the radio backend), signals, restart/stop requests |
| `src/ffs.c`, `pktq.c` | FunctionFS plumbing: descriptor and string blobs, IN-endpoint writer with the stuck-transfer watchdog, packet queue |
| `src/hci.c`, `acl_split.c`, `sco.c` | Bluetooth: HCI relay, ACL re-framing, call audio (isochronous, alternate-setting logic, pacing) |
| `src/wlan.c`, `bdc.c`, `fw.c`, `wl.h` | WiFi: USB side, BDC framing, the emulated Broadcom firmware (station and hotspot state machines) |
| `src/backend_sup.c`, `wpa_parse.c`, `tap.c` | the real radio: wpa_supplicant and hostapd drivers, parsing, the Ethernet tap |
| `src/backend_fake.c` | synthetic networks (`--wlan-backend fake`), no radio touched |
| `src/update.c`, `sha256.c` | update-over-USB protocol and maintenance-mode request (receiving side) |
| `tools/flash.c` | the PC side of the same protocol |
| `tests/` | one test program per area; `check.h` is the tiny test harness |
| `dist/` | `pi-radio-gadget-setup.sh` (configfs gadget), `pi-radio-gadget-update.sh` (SD drop-in and rollback), `pi-radio-gadget-radio.sh` (radio hand-back), unit, installer, udev rule |

## Iterating on hardware

The fast loop uses the USB update and maintenance mode, so the SD card stays in the Pi:

```
scripts/cross-build.sh && sudo build/pi-radio-gadget-flash build-aarch64/pi-radio-gadget   # new build running in ~10 s
sudo build/pi-radio-gadget-flash --stop                                                    # SSH back; then edit /etc/default, install scripts, read logs
# on the Pi: sudo rm /boot/firmware/pi-radio-gadget.disable && sudo systemctl start pi-radio-gadget
```

Try the USB side without touching the Pi's radio (SSH keeps working) with the fake backend:

```
sudo dist/pi-radio-gadget-setup.sh up
sudo build/pi-radio-gadget --udc-file /sys/kernel/config/usb_gadget/pi-radio-gadget/UDC --wlan-backend fake -vv
sudo dist/pi-radio-gadget-setup.sh down
```

Run the daemon with `-v` while diagnosing: dcmd-level logging of everything the PC's driver asks the emulated firmware.
`--status` (see OPERATIONS.md) is the first thing to look at for audio and WiFi data-path problems.

### Reading a usbmon capture

`scripts/usbmon-capture.sh SECONDS FILE` writes lines such as `ffff8ee... 2829476932 C Bi:1:097:1 0 76 = ...`:
tag, microsecond timestamp, `S`ubmit or `C`omplete, then type/direction (`B` bulk, `I` interrupt, `Z` isochronous, `C` control;
`i`/`o` = device-to-host / host-to-device), bus, device and endpoint number, then status and length. Useful greps:

```
grep ' Zi:' file | head      # call audio towards the PC
awk '$3=="C" && $4 ~ /Zo/' file | wc -l
grep -vE ' Zi:| Zo:' file | tail     # the last non-audio events before a stall
```

The device number changes at every re-enumeration; a capture is only valid until the Pi resets its USB link.

## Reference sources

Most of the design came from reading the PC-side drivers, not from datasheets. When something behaves oddly, the answer is usually in:

* `drivers/net/wireless/broadcom/brcm80211/brcmfmac/` (`cfg80211.c`, `feature.c`, `fwil*.h`, `bcdc.c`): what the emulated firmware must answer
* `drivers/bluetooth/btusb.c`: how the PC drives the Bluetooth interfaces and the isochronous alternate settings
* `drivers/usb/gadget/function/f_fs.c`, `drivers/usb/dwc2/gadget.c`: how FunctionFS and the Pi's USB controller behave
* hostapd/wpa_supplicant `src/drivers/driver_nl80211.c` and `src/ap/`: what the PC's access-point code expects from a driver

## Hardware and driver quirks learned the hard way

Each of these cost real debugging time; they are the reason several parts of the code look the way they do.

**USB gadget (dwc2 and FunctionFS)**
* The Pi Zero 2 W's `dwc2` has 7 device endpoints and runs in internal-DMA mode (not changeable from the device tree). An emulation
  needing 8 endpoints cannot work (that killed the earlier MediaTek dongle idea).
* FunctionFS re-enables **all** endpoints of a function whenever the host selects any alternate setting. A function with an
  alt-setting interface must therefore contain only that interface; otherwise every audio start resets the other endpoints.
* FunctionFS never reports which alternate setting was selected, and rejects one endpoint in two alternate settings, so each setting
  has its own endpoint pair and the daemon infers the active one from the Bluetooth commands.
* Isochronous IN: every queued request gets its own USB frame, and a late request completes with `ENODATA`. Requests must be queued at
  the frame rate, empty ones included (`sco_pace_writer`, AIO).
* Isochronous OUT: a read longer than `wMaxPacketSize` fails with `EINVAL`; `ENODATA` just marks an empty frame (retry at once).
* A bulk IN write that starts after the endpoint sat idle can hang (the USB stall, ARCHITECTURE.md §5). A control transfer in flight
  at the same moment makes it worse, hence the process-wide I/O lock.
* Interface numbering is load-bearing: the WiFi function must be interface 0, the audio function must directly follow the Bluetooth one.

**brcmfmac (the PC's WiFi driver, against the emulated firmware)**
* Answering the bootloader version request with chip id `0xA123` makes the driver skip the firmware download.
* `chanspecs` with a 40 MHz filter must return an empty list; `per_chan_info` must succeed; the iovars `sup_wpa` and `mfp` must exist,
  or PSK offload and PMF are silently disabled.
* The feature string `idauth` switches on AP PSK offload; `sae` switches on both station and AP SAE offload.
* `struct brcmf_sta_info_le` is **not packed** in the kernel: fields sit at natural alignment (flags at offset 8, not 6).
  Wrong offsets make every station look unassociated and infinitely idle.
* The driver sends the AP's association-request elements only through the `ASSOC_IND` event data; cfg80211 hands them to the PC's
  hostapd, which validates them, so they have to be plausible.

**Bluetooth chip (BCM43436 over UART)**
* After the controller is closed and reopened it stays mute until `hci_uart` is reloaded; the setup script reloads it at every start.
* Call audio goes to the unconnected PCM pins unless the vendor command `0xFC1C` routes it over HCI; it must be re-sent after every HCI reset.
* `btusb` discards a whole SCO packet if any of its USB frames is missing, so each frame carries one complete packet (63 bytes);
  smaller frames lost half the audio.

**The Pi's WiFi chip**
* Stock firmware: managed, AP, IBSS and P2P; no monitor mode, 2.4 GHz only. Power saving is on by default and is switched off by the daemon.
* A hotspot configured with a raw 32-byte PSK works with hostapd's `wpa_psk=<hex>`, so the Pi never needs the passphrase.

**The PC side**
* NixOS's firewall drops hotspot clients' DHCP/DNS (see SETUP.md and `contrib/nixos/`).

## History

The project replaced two earlier prototypes (a Python HCI relay, and an emulated MediaTek MT7601U WiFi dongle) with a single C daemon
that emulates a Broadcom FullMAC dongle. Chronology of the capabilities: composite gadget with Bluetooth relay and the WiFi function; real-radio
backend; update over USB with rollback; call audio (SCO) including the move of the audio interface into its own USB function;
hotspot mode and maintenance mode.
