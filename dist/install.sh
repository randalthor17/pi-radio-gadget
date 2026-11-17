#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Installs pi-radio-gadget on the Raspberry Pi: builds it here, or installs a binary built elsewhere.
#
#   sudo ./dist/install.sh                       build + install, leave the service disabled
#   sudo ./dist/install.sh --enable              ... and start it at every boot
#   sudo ./dist/install.sh --binary FILE         install a prebuilt static aarch64 binary (scripts/cross-build.sh); no build tools needed
#   sudo ./dist/install.sh --uninstall
#
# Building needs: gcc, meson, ninja  (apt install gcc meson ninja-build)
# One-time prerequisite: dtoverlay=dwc2 in /boot/firmware/config.txt, then reboot.
#
# An installation made under the project's earlier name (pi-radiogadget, before 0.4.0) is migrated: its service is stopped and
# removed, its settings are carried over, and the new service takes its place.
set -eu

PREFIX=/opt/pi-radio-gadget
HERE="$(cd "$(dirname "$0")/.." && pwd)"
BOOT=/boot/firmware
ENABLE=0
BINARY=
ACTION=install
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }

usage() { echo "usage: $0 [--enable] [--binary FILE] | --uninstall" >&2; exit 1; }
while [ $# -gt 0 ]; do
    case "$1" in
        --enable) ENABLE=1 ;;
        --uninstall) ACTION=uninstall ;;
        --binary) shift; [ $# -gt 0 ] || usage; BINARY=$1 ;;
        *) usage ;;
    esac
    shift
done

uninstall() {
    systemctl disable --now pi-radio-gadget 2>/dev/null || true
    rm -f /etc/systemd/system/pi-radio-gadget.service
    systemctl daemon-reload
    rm -rf "$PREFIX"
    echo "removed $PREFIX (kept /etc/default/pi-radio-gadget)"
}

# Carries an installation under the old name over to the new one.
migrate_legacy() {
    local old=/opt/pi-radiogadget
    if [ -e /etc/systemd/system/pi-radiogadget.service ] || [ -d "$old" ]; then
        echo "migrating the installation made under the old name (pi-radiogadget)"
        systemctl disable --now pi-radiogadget 2>/dev/null || true
        # its radio hand-back (a daemon killed mid-run leaves the interface unmanaged)
        [ -x "$old/pi-radiogadget-radio.sh" ] && "$old/pi-radiogadget-radio.sh" restore 2>/dev/null || true
        rm -f /etc/systemd/system/pi-radiogadget.service
        rm -rf "$old"
    fi
    if [ -f /etc/default/pi-radiogadget ] && [ ! -f /etc/default/pi-radio-gadget ]; then
        sed 's/PI_RADIOGADGET/PI_RADIO_GADGET/g' /etc/default/pi-radiogadget > /etc/default/pi-radio-gadget
        echo "settings carried over to /etc/default/pi-radio-gadget"
    fi
    rm -f /etc/default/pi-radiogadget /etc/default/pi-radiogadget.bak
    rm -f "$BOOT"/pi-radiogadget.disable "$BOOT"/pi-radiogadget.log "$BOOT"/pi-radiogadget-hostapd.log
    rm -rf "$BOOT"/pi-radiogadget-update "$BOOT"/pi-radiogadget-update.applied
    systemctl daemon-reload
}

if [ "$ACTION" = uninstall ]; then
    uninstall
    exit 0
fi

grep -q '^dtoverlay=dwc2' $BOOT/config.txt 2>/dev/null ||
    echo "WARNING: dtoverlay=dwc2 is not set in $BOOT/config.txt; the USB gadget cannot work until it is."

command -v hostapd >/dev/null ||
    echo "NOTE: hostapd is not installed: the PC cannot run a WiFi hotspot through the Pi until you  apt install hostapd  (it stays disabled as a system service; this daemon starts it itself)."

if [ -n "$BINARY" ]; then
    [ -f "$BINARY" ] || { echo "no such file: $BINARY" >&2; exit 1; }
    BUILT="$BINARY"
else
    for t in gcc meson ninja; do
        command -v "$t" >/dev/null || { echo "missing build tool: $t  (apt install gcc meson ninja-build)" >&2; exit 1; }
    done
    BUILD="$(mktemp -d)"
    trap 'rm -rf "$BUILD"' EXIT
    meson setup "$BUILD" "$HERE" --prefix="$PREFIX" --bindir=. -Dbuildtype=release >/dev/null
    ninja -C "$BUILD" >/dev/null
    meson test -C "$BUILD" >/dev/null || { echo "unit tests failed; not installing" >&2; exit 1; }
    BUILT="$BUILD/pi-radio-gadget"
fi

migrate_legacy

install -d "$PREFIX"
install -m 755 "$BUILT" "$PREFIX/pi-radio-gadget"
install -m 755 "$HERE/dist/pi-radio-gadget-setup.sh" "$PREFIX/pi-radio-gadget-setup.sh"
install -m 755 "$HERE/dist/pi-radio-gadget-radio.sh" "$PREFIX/pi-radio-gadget-radio.sh"
install -m 755 "$HERE/dist/pi-radio-gadget-update.sh" "$PREFIX/pi-radio-gadget-update.sh"
[ -f "$HERE/README.md" ] && install -m 644 "$HERE/README.md" "$PREFIX/README.md"
install -m 644 "$HERE/dist/pi-radio-gadget.service" /etc/systemd/system/pi-radio-gadget.service
[ -f /etc/default/pi-radio-gadget ] || install -m 644 "$HERE/dist/pi-radio-gadget.default" /etc/default/pi-radio-gadget
systemctl daemon-reload
echo "installed to $PREFIX"

if [ "$ENABLE" = 1 ]; then
    # The earlier prototype services use the same USB controller.
    systemctl disable btgadget.service wifigadget.service 2>/dev/null || true
    systemctl enable pi-radio-gadget.service
    echo "enabled: pi-radio-gadget starts at boot. Start it now with: systemctl start pi-radio-gadget"
    echo "NOTE: while it runs, the Pi's own WiFi belongs to the PC (no SSH over WiFi)."
else
    echo "service not enabled. Try it with: systemctl start pi-radio-gadget   (stop: systemctl stop pi-radio-gadget)"
    echo "Make it permanent with: $0 --enable"
fi
