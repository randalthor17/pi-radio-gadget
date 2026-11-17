#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Records the USB traffic between this PC and the Pi gadget (usbmon text format) for debugging stalls and audio.
#
#   sudo scripts/usbmon-capture.sh SECONDS OUTPUT_FILE
#
# The device number changes on every re-enumeration, so a capture ends when the Pi resets its USB link; start another one.
# Reading: "Zi/Zo" isochronous (call audio), "Bi/Bo" bulk, "Ii" interrupt (HCI events), "Ci/Co" control.
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run as root (usbmon needs it)" >&2; exit 1; }
[ $# = 2 ] || { echo "usage: $0 SECONDS OUTPUT_FILE" >&2; exit 2; }
secs=$1; out=$2
modprobe usbmon 2>/dev/null || true
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
dev=
for d in /sys/bus/usb/devices/*; do
    [ -f "$d/idVendor" ] && [ "$(cat "$d/idVendor")" = 0a5c ] && [ "$(cat "$d/idProduct")" = 0bdc ] && { dev=$(cat "$d/devnum"); bus=$(cat "$d/busnum"); }
done
[ -n "$dev" ] || { echo "Pi gadget (0a5c:0bdc) not found" >&2; exit 1; }
echo "capturing device $dev on bus $bus for $secs s -> $out" >&2
timeout "$secs" cat "/sys/kernel/debug/usb/usbmon/${bus}u" | awk -v dev="$dev" '{ split($4, a, ":"); if (a[3] + 0 == dev) print }' > "$out" || true
wc -l "$out" >&2
