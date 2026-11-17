#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Creates / removes the composite USB gadget (configfs) for pi-radio-gadget.
#
#   pi-radio-gadget-setup.sh up|down
#
# The gadget has three FunctionFS functions. The ORDER matters:
#   1. ffs.wlan  must come first: it becomes USB interface 0, because the PC's brcmfmac driver
#                sends its probe-time bootloader requests to interface 0.
#   2. ffs.hci   second: it claims device-recipient class requests (HCI commands), which the
#                kernel hands to the first function that asks for them.
#   3. ffs.sco   third: the Bluetooth call-audio interface; btusb expects it right after the HCI one.
# The daemon (pi-radio-gadget) writes the function descriptors and attaches the UDC itself.
#
# One-time prerequisite: dtoverlay=dwc2 in /boot/firmware/config.txt. Plug the PC into the
# Pi's DATA micro-USB port (the middle one), not the PWR port.
set -eu

G=/sys/kernel/config/usb_gadget/pi-radio-gadget
FFS_WLAN=/dev/ffs-wlan
FFS_HCI=/dev/ffs-hci
FFS_SCO=/dev/ffs-sco
ENABLE_WLAN="${PI_RADIO_GADGET_WLAN:-1}"
ENABLE_HCI="${PI_RADIO_GADGET_HCI:-1}"

[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }

detach_udc() {
    local g="$1"
    [ -e "$g/UDC" ] && echo "" > "$g/UDC" 2>/dev/null || true
}

# Tears down one gadget directory, whatever functions it holds.
remove_gadget() {
    local g="$1"
    [ -d "$g" ] || return 0
    detach_udc "$g"
    local l
    for l in "$g"/configs/*/*; do [ -L "$l" ] && rm -f "$l"; done
    for l in "$g"/configs/*/strings/*; do [ -d "$l" ] && rmdir "$l" 2>/dev/null || true; done
    for l in "$g"/configs/*; do [ -d "$l" ] && rmdir "$l" 2>/dev/null || true; done
    for l in "$g"/functions/*; do [ -d "$l" ] && rmdir "$l" 2>/dev/null || true; done
    for l in "$g"/strings/*; do [ -d "$l" ] && rmdir "$l" 2>/dev/null || true; done
    rmdir "$g" 2>/dev/null || true
}

unmount_ffs() {
    local d
    for d in "$FFS_WLAN" "$FFS_HCI" "$FFS_SCO"; do
        mountpoint -q "$d" && umount "$d" 2>/dev/null || true
    done
}

down() {
    detach_udc "$G"
    unmount_ffs
    remove_gadget "$G"
    # The earlier prototypes' gadgets would hold the UDC: clear them too.
    remove_gadget /sys/kernel/config/usb_gadget/hcidongle
    remove_gadget /sys/kernel/config/usb_gadget/mt7601u
}

up() {
    down                                   # always start from a clean slate

    modprobe dwc2 2>/dev/null || true
    modprobe libcomposite

    if [ "$ENABLE_HCI" = 1 ]; then
        # User-channel HCI needs the controller free of bluetoothd.
        systemctl stop bluetooth 2>/dev/null || true
        # The Pi's UART Bluetooth controller does not survive a close/reopen cycle: after the daemon
        # exits (or restarts) it comes back mute until the driver is reloaded, which re-runs the firmware
        # patch and baud-rate setup. So always start from a freshly loaded driver.
        modprobe -r hci_uart 2>/dev/null || true
        modprobe hci_uart 2>/dev/null || true
        for _ in $(seq 1 100); do [ -e /sys/class/bluetooth/hci0 ] && break; sleep 0.1; done
        [ -e /sys/class/bluetooth/hci0 ] || { echo "hci0 not found" >&2; exit 1; }
    fi

    mkdir -p "$G"
    cd "$G"
    # IDs of Broadcom's "BCM dongle with firmware already running": brcmfmac then skips the firmware
    # download. Class EF/02/01 (misc, interface association): both function drivers match per interface.
    echo 0x0a5c > idVendor
    echo 0x0bdc > idProduct
    echo 0x0100 > bcdDevice
    echo 0x0200 > bcdUSB
    # Optional speed cap: low-speed|full-speed|high-speed (default: the controller maximum).
    [ -n "${PI_RADIO_GADGET_MAX_SPEED:-}" ] && echo "$PI_RADIO_GADGET_MAX_SPEED" > max_speed
    echo 0xef > bDeviceClass
    echo 0x02 > bDeviceSubClass
    echo 0x01 > bDeviceProtocol

    local serial
    serial="$(awk '/^Serial/ {print $3}' /proc/cpuinfo 2>/dev/null || true)"
    mkdir -p strings/0x409
    echo "${serial:-0000000001}" > strings/0x409/serialnumber
    echo "Raspberry Pi"          > strings/0x409/manufacturer
    echo "Pi WiFi + Bluetooth"   > strings/0x409/product

    mkdir -p configs/c.1/strings/0x409
    echo "WiFi + Bluetooth" > configs/c.1/strings/0x409/configuration
    echo 100  > configs/c.1/MaxPower
    echo 0x80 > configs/c.1/bmAttributes

    # Functions are linked in this order on purpose (see the header comment).
    if [ "$ENABLE_WLAN" = 1 ]; then
        mkdir -p functions/ffs.wlan
        ln -s "$G/functions/ffs.wlan" configs/c.1/ffs.wlan
        mkdir -p "$FFS_WLAN"
        mount -t functionfs wlan "$FFS_WLAN"
    fi
    if [ "$ENABLE_HCI" = 1 ]; then
        mkdir -p functions/ffs.hci
        ln -s "$G/functions/ffs.hci" configs/c.1/ffs.hci
        mkdir -p "$FFS_HCI"
        mount -t functionfs hci "$FFS_HCI"
        # Call audio (isochronous SCO, USB interface 2) is a function of its own: FunctionFS re-enables every endpoint of a
        # function when the host selects an alternate setting, which must not disturb the Bluetooth control/ACL endpoints.
        mkdir -p functions/ffs.sco
        ln -s "$G/functions/ffs.sco" configs/c.1/ffs.sco
        mkdir -p "$FFS_SCO"
        mount -t functionfs sco "$FFS_SCO"
    fi
    echo "gadget prepared at $G"
}

case "${1:-}" in
    up)   up ;;
    down) down ;;
    *)    echo "usage: $0 up|down" >&2; exit 1 ;;
esac
