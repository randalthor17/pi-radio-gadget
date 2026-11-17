#!/bin/bash
# Gives the Pi's WiFi interface back to the system after pi-radio-gadget has used it.
#
#   pi-radio-gadget-radio.sh restore
#
# The daemon does this itself on a clean exit. This script exists for the other exits (crash,
# SIGKILL, power-fail mid-run): systemd runs it from ExecStopPost, so it must be idempotent.
set -u
IF="${PI_RADIO_GADGET_IFACE:-wlan0}"

[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }

restore() {
    pkill -f "wpa_supplicant -i $IF -D nl80211 -C /run/pi-radio-gadget" 2>/dev/null || true
    pkill -f "hostapd .*/run/pi-radio-gadget" 2>/dev/null || true     # a hotspot that outlived the daemon
    sleep 0.5

    # The PC may have changed the MAC address; put the factory one back.
    local perm cur
    perm="$(cat "/sys/class/net/$IF/phy80211/macaddress" 2>/dev/null || true)"
    cur="$(cat "/sys/class/net/$IF/address" 2>/dev/null || true)"
    if [ -n "$perm" ] && [ "$perm" != "$cur" ]; then
        ip link set dev "$IF" down 2>/dev/null || true
        ip link set dev "$IF" address "$perm" 2>/dev/null || true
        ip link set dev "$IF" up 2>/dev/null || true
    fi
    ip link set dev "$IF" allmulticast off 2>/dev/null || true
    sysctl -qw "net.ipv6.conf.$IF.disable_ipv6=0" 2>/dev/null || true

    if command -v nmcli >/dev/null 2>&1; then
        nmcli device set "$IF" managed yes 2>/dev/null || true
        nmcli --wait 0 device connect "$IF" >/dev/null 2>&1 || true
    fi
    rm -rf /run/pi-radio-gadget
}

case "${1:-}" in
    restore) restore ;;
    *) echo "usage: $0 restore" >&2; exit 1 ;;
esac
