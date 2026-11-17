# Setup: installing and applying pi-radio-gadget

## What you need

* A **Raspberry Pi Zero 2 W** with Raspberry Pi OS (Debian 13 "trixie" tested), with its normal WiFi working for the first install.
* A USB cable that carries **data**, from the Pi's **middle** micro-USB port (labelled USB, not PWR) to the PC. The Pi can be powered
  from the PWR port or through the data cable.
* A Linux PC with the standard `brcmfmac` and `btusb` kernel modules (every mainstream distribution) and NetworkManager, `iwd`
  or plain `wpa_supplicant`. Nothing is installed on the PC for normal use.

On the Pi, at run time the daemon uses `wpa_supplicant` (present on Raspberry Pi OS), `nmcli` (to take the interface from
NetworkManager and give it back), `iw` (to switch off radio power saving) and, for the hotspot, `hostapd`.

## 1. Prepare the Pi (once)

1. Enable the USB device controller: add this under `[all]` in `/boot/firmware/config.txt`, then reboot.
   ```
   dtoverlay=dwc2
   ```
2. Install what the build and the hotspot need:
   ```
   sudo apt install gcc meson ninja-build hostapd
   ```
   Debian installs `hostapd` as a masked system service on purpose: the daemon starts it itself when the PC asks for a hotspot.
   Skip `hostapd` if you do not want hotspots.

## 2. Install the daemon

### A. Build on the Pi (simplest)

Copy this directory to the Pi (`scp -r`, `git clone`, ...), then on the Pi:

```
sudo ./dist/install.sh            # build, run the unit tests, install to /opt/pi-radiogadget, leave the service disabled
sudo systemctl start pi-radiogadget
```

Try it now (next section). When you are happy, make it start at every boot:

```
sudo ./dist/install.sh --enable
```

`install.sh --uninstall` removes the daemon and the unit again (it keeps `/etc/default/pi-radiogadget`).

### B. Cross-build on the PC and copy it over

```
scripts/cross-build.sh                      # -> build-aarch64/pi-radiogadget (static aarch64, needs nix or an aarch64 cross compiler)
```

The first installation of a binary still needs a way onto the Pi: `scp` it and install it by hand, or run `install.sh` on the Pi once
(option A). After that, every later build is pushed over the USB cable, see [OPERATIONS.md](OPERATIONS.md).

## 3. First run

```
sudo systemctl start pi-radiogadget        # on the Pi; its own WiFi/SSH disappear now, that is expected
```

On the PC, within about 25 s, a WiFi interface and a Bluetooth controller appear (`nmcli device`, `bluetoothctl list`).

```
nmcli device wifi connect "MyNetwork" password "..."
ping -I wlan0 1.1.1.1
bluetoothctl                               # power on, scan on, pair <MAC>, connect <MAC>
```

If nothing appears, or you need the Pi's SSH back, see [OPERATIONS.md](OPERATIONS.md): *Maintenance mode* (from the PC, over the cable) and *Recovery* (from the SD card).

## 4. The PC side

Normal use needs nothing. Optional pieces:

* **The flash/status tool** (`pi-radiogadget-flash`, built with `meson setup build && ninja -C build`, binary in `build/`) talks to the
  device through `/dev/bus/usb`. Run it as root, or install the udev rule so your logged-in user may:
  ```
  sudo cp dist/70-pi-radiogadget.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger
  ```
  On NixOS put the rule's content in `services.udev.extraRules`.
* **Hotspots and your firewall.** A hotspot (NetworkManager "shared" mode) runs a DHCP/DNS server on the PC. Many firewalls drop
  requests from the hotspot's clients, which shows up as the phone joining the network but failing to get an IP address.
  * Fedora, RHEL and others with firewalld: NetworkManager handles it (zone `nm-shared`); nothing to do.
  * Debian/Ubuntu without a firewall: nothing to do. With `ufw`: allow UDP 67 and 53, TCP 53 on the hotspot interface.
  * **NixOS:** import [`contrib/nixos/nm-shared-firewall.nix`](../contrib/nixos/nm-shared-firewall.nix). It opens those ports only while a
    shared connection is up. Or, simpler and always on:
    ```nix
    networking.firewall.interfaces.wlan0.allowedUDPPorts = [ 53 67 ];
    networking.firewall.interfaces.wlan0.allowedTCPPorts = [ 53 ];
    ```
    (The interface name is whatever `ip link` shows for the dongle.)
* **Call audio on the PC:** PipeWire/WirePlumber switches the headset to the "Headset Head Unit (HFP/HSP)" profile when a call
  starts (or by hand in the sound settings). mSBC (wideband) is used when the headset offers it.

## 5. Configuration

`/etc/default/pi-radiogadget` (created by the installer if missing):

| Variable | Meaning |
|---|---|
| `PI_RADIOGADGET_WLAN=1/0` | expose the WiFi function |
| `PI_RADIOGADGET_HCI=1/0` | expose the Bluetooth function |
| `PI_RADIOGADGET_ARGS` | extra daemon options (`pi-radiogadget --help`). The default is `--sco-over-hci`, which Bluetooth **call audio needs**. `-v`/`-vv` raise the log level, `--wlan-iface`, `--hci-dev`, `--keepalive-ms` and others exist |
| `PI_RADIOGADGET_MAX_SPEED` | `full-speed` caps the USB link at 12 Mbit/s (an experiment knob, not a fix for anything known) |

Restart after changing it: `sudo systemctl restart pi-radiogadget`.

## 6. Removing it

```
sudo ./dist/install.sh --uninstall
```

On the PC nothing was installed, so there is nothing to remove.
