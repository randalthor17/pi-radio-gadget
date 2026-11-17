# Operations: using, updating, recovering and diagnosing

## Daily use

Plug the Pi in and wait about 25 s. The PC gets a WiFi interface and a Bluetooth controller.

* **WiFi:** use it as any adapter (`nmcli`, your desktop's network menu). NetworkManager's MAC randomization is honoured: the
  Pi's radio adopts the address and the factory address is restored when the service stops.
* **Bluetooth:** `bluetoothctl` or your desktop's Bluetooth panel. Calls need the `--sco-over-hci` daemon option (the default
  in `/etc/default/pi-radiogadget`); the headset switches to the call profile when a call starts.
* **Hotspot:** `nmcli device wifi hotspot ifname wlan0 ssid MyHotspot password "..." band bg channel 6` (or hostapd on the PC).
  The PC shares its internet connection; the Pi's radio is the access point. Stop it with `nmcli connection down Hotspot` (or the
  connection's name). One radio means a hotspot and a normal WiFi connection cannot run at the same time. The PC's firewall must
  allow DHCP/DNS from the clients, see [SETUP.md](SETUP.md).

## Updating the daemon over the USB cable

No SD card, no network. Needs a daemon of version 0.3.0 or later on the Pi.

```
scripts/cross-build.sh                                   # or build any other way: a static aarch64 binary
build/pi-radiogadget-flash build-aarch64/pi-radiogadget  # as root, or with the udev rule from SETUP.md
```

What happens: the tool sends the binary through the gadget's control endpoint; the Pi checks its length, SHA-256 and ELF header,
keeps the old binary as `/opt/pi-radiogadget/pi-radiogadget.prev`, replaces the binary atomically, writes
`/opt/pi-radiogadget/pending` and restarts (the USB device disappears for a few seconds). The new daemon deletes `pending` after
60 s of uptime. If it dies before that, the service script counts the failed starts and **rolls back to `.prev` after two**.

There is no signature: anyone with access to the USB device can replace the daemon, which means only the PC the Pi is plugged
into. This was a deliberate choice (a lost signing key would lock you out of your own device).

The tool checks that the file is an aarch64 ELF; `--force` skips that check.

### Updating scripts and the service unit

The USB update carries the binary only. Changes to `dist/*.sh` or the service file go through the SD card (below) or over SSH
(maintenance mode). A change of the USB layout (it happened once, when the audio interface became its own function) needs the
binary **and** `pi-radiogadget-setup.sh` together, so use the SD card for those.

## Maintenance mode: SSH into the Pi without opening it

```
pi-radiogadget-flash --stop
```

The daemon creates `/boot/firmware/pi-radiogadget.disable`, hands the WiFi interface back to the system and exits. The Pi joins its
own WiFi again (a few seconds later) and `ssh` works. The service stays disabled, also across reboots. To end maintenance:

```
ssh pi@<address>
sudo rm /boot/firmware/pi-radiogadget.disable && sudo systemctl start pi-radiogadget
```

Handy for development: edit, cross-build, `scp`, restart, test, without touching the SD card.

## Recovery: the SD card

Every one of these works from any PC that can mount the SD card's boot partition (the FAT partition; `/boot/firmware/` on the Pi):

* **Stop the service from starting:** create an empty file `pi-radiogadget.disable`. The Pi's WiFi and SSH then work normally.
  Delete the file to re-enable.
* **Update without any connection:** put the new files into a folder `pi-radiogadget-update/` on the boot partition: any of the
  `pi-radiogadget` binary (static aarch64), `*.sh` scripts, `pi-radiogadget.service`, `pi-radiogadget.default` (only used if
  `/etc/default/pi-radiogadget` is missing). The next service start installs them and renames the folder to
  `pi-radiogadget-update.applied`.
* **Read what happened:** the daemon mirrors its log to `pi-radiogadget.log` on the boot partition (INFO and above, capped at
  256 KB, appended across restarts with a `---- start ----` marker), and hostapd's own output goes to
  `pi-radiogadget-hostapd.log`.

Automatic safety nets: if the daemon is killed or crashes, `ExecStopPost` hands the WiFi interface back to NetworkManager and
restores its MAC address. The service restarts itself after a failure and gives up after 6 failures in 10 minutes, which leaves the
Pi's WiFi/SSH available for diagnosis.

## Diagnosing

### The status line

```
pi-radiogadget-flash --status
```

prints the version and live counters, for example:

```
pi-radiogadget 0.4.0 (...) state=idle got=0/0 pending=0 last="idle" sco[alt=1 chip_pkts=20194 to_chip=20228 a1(...) a2(...)]
rd[a1=20228/131542/61 ...] acl[to_chip=... from_chip=...] wifi[hotspot stations=1 to_host=45180 from_host=35086 drop_to_host=0
drop_from_host=0] tap[rx=45180 tx=35079 tx_drop=0 eapol_drop=2 sa_rewrites=0 enabled=1] radio=hotspot
```

| Field | Meaning |
|---|---|
| `pending` | 1 while a freshly installed binary has not yet run 60 s |
| `sco[alt= chip_pkts= to_chip=]` | which audio alternate setting is active, SCO packets from the Bluetooth chip towards the PC, and from the PC to the chip |
| `a1(...)`, `a2(...)` | per alternate setting: `data` audio packets written to the PC, `lost` ones that missed their USB frame, `idle` empty keep-rhythm frames, `err=count/errno` |
| `rd[a1=reads/errors/errno]` | reads of the PC's audio frames; errno 61 (`ENODATA`) just means "no audio in that frame" |
| `wifi[state ...]` | the emulated firmware: `idle`, `joining`, `station` or `hotspot`, connected hotspot stations, frames each way and dropped ones |
| `tap[...]` | frames through the Pi's radio interface; `eapol_drop` counts handshake frames kept off the PC |
| `radio=` | who owns the radio right now |

### Logs

* On the SD card: `pi-radiogadget.log`, `pi-radiogadget-hostapd.log` (see above).
* On the Pi: `journalctl -u pi-radiogadget` (add `-v` or `-vv` to `PI_RADIOGADGET_ARGS` for per-request detail).
* On the PC: `journalctl -k` shows `brcmfmac`, `btusb` and USB messages. A USB reset of the gadget appears as
  `usb 1-x: USB disconnect` followed by a new device.

### Capturing USB traffic

```
sudo scripts/usbmon-capture.sh 90 /tmp/capture.txt
```

records the PC's view of the gadget's USB traffic (usbmon text format) for 90 s. See [DEVELOPMENT.md](DEVELOPMENT.md) for how to read it.

## Troubleshooting

| Symptom | Likely cause and what to do |
|---|---|
| Nothing appears on the PC | Cable is power-only or in the wrong port (use the middle one). `dtoverlay=dwc2` missing. Service not running (`systemctl status pi-radiogadget`, or the `.disable` file exists). Wait a full 25 s after power-up |
| WiFi interface appears, scans fail or the interface vanishes now and then | The USB stall (ARCHITECTURE.md §5): the daemon reconnects the USB link; WiFi and Bluetooth reappear after about 3 s. A shorter, good-quality data cable and a different PC port are worth trying |
| Bluetooth works, calls carry no sound | `--sco-over-hci` is missing from `PI_RADIOGADGET_ARGS`. Check `sco[...]` in `--status`: `to_chip` rising means the PC's audio reaches the chip |
| Hotspot appears but phones cannot get an IP address | The PC's firewall blocks DHCP/DNS from the hotspot interface, see [SETUP.md](SETUP.md). Check `tcpdump -i wlan0 port 67` on the PC: requests arriving but no reply means the firewall |
| Hotspot does not appear at all | `hostapd` missing on the Pi, or it failed: read `pi-radiogadget-hostapd.log`. WPA3 and other unsupported modes are refused (the daemon log says why) |
| Phones can join but disconnect after a few minutes | Not seen since the `sta_info` fix. If it recurs, capture `iw dev wlan0 station dump` on the PC and the daemon log |
| The Pi's WiFi never comes back after stopping the service | Run `sudo /opt/pi-radiogadget/pi-radiogadget-radio.sh restore` on the Pi, or reboot |
| An update left the Pi stuck | It rolls back by itself after two failed starts. Otherwise use the SD card: create `pi-radiogadget.disable`, or drop a known-good binary into `pi-radiogadget-update/` |
