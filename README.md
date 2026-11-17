# pi-radio-gadget

Turn a **Raspberry Pi Zero 2 W** into a **USB WiFi + Bluetooth dongle** for a Linux PC.

Plug the Pi's USB *data* port into the PC and the PC sees an ordinary WiFi adapter and an ordinary Bluetooth
controller. Pick a network in NetworkManager, pair a headset with `bluetoothctl`, take a call, start a hotspot: the PC does
the thinking, the Pi's own radio does the radio work. Nothing needs to be installed on the PC, no driver and no firmware.

```
 PC (stock Linux)                          Raspberry Pi Zero 2 W
 ┌─────────────────────────┐  one USB     ┌─────────────────────────────────┐
 │ NetworkManager          │  cable       │ pi-radiogadget daemon           │
 │ brcmfmac  ─────────────────────────────┼─► emulated Broadcom dongle ─────┼─► Pi's WiFi chip
 │ btusb / bluetoothd ────────────────────┼─► HCI + call-audio relay ───────┼─► Pi's Bluetooth chip
 └─────────────────────────┘              └─────────────────────────────────┘
```

(The repository is `pi-radio-gadget`; the daemon, service and tools keep the name `pi-radiogadget`.)

## What works

| Feature | Status |
|---|---|
| WiFi: scan, connect, roam, disconnect, MAC randomization | works, 2.4 GHz only (the chip has no 5 GHz) |
| WiFi security | open, WPA, WPA2-PSK, WPA3-SAE, PMF. WPA3 and hidden networks are implemented but not verified against real access points. WPA-Enterprise (802.1X) and WEP are refused with a log message |
| **WiFi hotspot** (NetworkManager "Hotspot" / hostapd on the PC) | works: open and WPA2-PSK, 2.4 GHz; tested with a phone at 27 Mbit/s down, 17 Mbit/s up. Needs `hostapd` on the Pi. No WPA3, and not at the same time as a station connection |
| Bluetooth: BLE, A2DP, HID, file transfer, everything on ACL | works |
| **Bluetooth call audio** (HFP over SCO, CVSD and mSBC) | works in both directions, tested with a headset on a call |
| Update the daemon over the USB cable | works (checksum, automatic rollback) |
| Monitor mode | **not possible**: the Pi's stock firmware cannot do it |
| WiFi Direct / P2P, 5 GHz | not available |

Throughput is bounded by the Pi's single-stream 802.11n chip and USB 2.0: tens of Mbit/s.

**Known limitation:** the Pi's USB controller (`dwc2`) occasionally leaves a transfer hanging. The daemon heals almost all
stalls and, as a last resort, reconnects the USB link (WiFi and Bluetooth vanish for about 3 s). Details and what was
tried: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) §5.

## Quick start

1. **Pi, once:** add `dtoverlay=dwc2` under `[all]` in `/boot/firmware/config.txt`, `sudo apt install gcc meson ninja-build hostapd`,
   reboot, then in this directory `sudo ./dist/install.sh --enable`.
2. **Cable:** USB *data* cable from the Pi's middle micro-USB port (labelled USB) to the PC. Wait about 25 s.
3. **PC:** a WiFi interface and a Bluetooth controller appear.
   ```
   nmcli device wifi connect "MyNetwork" password "..."
   bluetoothctl                     # power on, scan on, pair, connect
   nmcli device wifi hotspot ifname wlan0 ssid MyHotspot password "..."
   ```

> **While the service runs, the Pi's own WiFi belongs to the PC**, so there is no WiFi or SSH on the Pi itself. That is the
> point of the project. To get SSH back without opening the Pi, run `pi-radiogadget-flash --stop` on the PC
> ([docs/OPERATIONS.md](docs/OPERATIONS.md)).

## Documentation

| | |
|---|---|
| [docs/SETUP.md](docs/SETUP.md) | installing and applying it: the Pi, the PC, NixOS and firewall notes, configuration |
| [docs/OPERATIONS.md](docs/OPERATIONS.md) | day-to-day use, updating over USB, maintenance mode, recovery, diagnosing problems |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | how it works and why: the USB gadget, the emulated WiFi firmware, Bluetooth and call audio, the hotspot, the USB stall |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | building, testing, cross-compiling, debugging on hardware, driver and hardware quirks learned the hard way |

## Repository layout

| Path | Contents |
|---|---|
| `src/` | the daemon (C11, no dependencies besides libc and pthreads) |
| `tests/` | unit tests, no hardware needed (`meson test`) |
| `tools/flash.c` | `pi-radiogadget-flash`: PC-side tool to update or stop the daemon over USB, and to read its status |
| `dist/` | systemd unit, gadget setup script, installer, update/rollback script, udev rule |
| `scripts/` | cross-build for the Pi, USB traffic capture |
| `contrib/nixos/` | NixOS module that lets hotspot clients reach the PC's DHCP/DNS |
| `docs/` | documentation |

## Verification status

Verified on real hardware (Pi Zero 2 W and a Linux PC, kernel 6.18): composite USB enumeration; the PC's stock `brcmfmac`
bringing up the emulated dongle with no firmware file; scans, a WPA2 connection through the Pi with DHCP and sustained downloads
(about 28-36 Mbit/s); Bluetooth discovery and pairing; an mSBC call with audio both ways; a hotspot with a phone for minutes;
update over USB, including a deliberately broken update after which the previous version was running again (the rollback was inferred from the outcome, not watched); maintenance mode.

Not verified: WPA3 and hidden networks on real access points, WPA-Enterprise (not implemented), hotspots with several clients,
and very long sessions (hours) of anything.
