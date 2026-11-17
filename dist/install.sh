#!/bin/bash
# Builds and installs pi-radiogadget on the Raspberry Pi.
#
#   sudo ./dist/install.sh              build + install, leave the service disabled
#   sudo ./dist/install.sh --enable     ... and start it at every boot
#   sudo ./dist/install.sh --uninstall
#
# Needs: gcc, meson, ninja  (apt install gcc meson ninja-build)
# One-time prerequisite: dtoverlay=dwc2 in /boot/firmware/config.txt, then reboot.
set -eu

PREFIX=/opt/pi-radiogadget
HERE="$(cd "$(dirname "$0")/.." && pwd)"
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }

uninstall() {
    systemctl disable --now pi-radiogadget 2>/dev/null || true
    rm -f /etc/systemd/system/pi-radiogadget.service
    systemctl daemon-reload
    rm -rf "$PREFIX"
    echo "removed $PREFIX (kept /etc/default/pi-radiogadget)"
}

case "${1:-}" in
    --uninstall) uninstall; exit 0 ;;
    ""|--enable) ;;
    *) echo "usage: $0 [--enable|--uninstall]" >&2; exit 1 ;;
esac

grep -q '^dtoverlay=dwc2' /boot/firmware/config.txt 2>/dev/null ||
    echo "WARNING: dtoverlay=dwc2 is not set in /boot/firmware/config.txt; the USB gadget cannot work until it is."

for t in gcc meson ninja; do
    command -v "$t" >/dev/null || { echo "missing build tool: $t  (apt install gcc meson ninja-build)" >&2; exit 1; }
done

command -v hostapd >/dev/null ||
    echo "NOTE: hostapd is not installed: the PC cannot run a WiFi hotspot through the Pi until you  apt install hostapd  (it stays disabled as a system service; this daemon starts it itself)."

BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT
meson setup "$BUILD" "$HERE" --prefix="$PREFIX" --bindir=. -Dbuildtype=release >/dev/null
ninja -C "$BUILD" >/dev/null
meson test -C "$BUILD" >/dev/null || { echo "unit tests failed; not installing" >&2; exit 1; }

install -d "$PREFIX"
install -m 755 "$BUILD/pi-radiogadget" "$PREFIX/pi-radiogadget"
install -m 755 "$HERE/dist/pi-radiogadget-setup.sh" "$PREFIX/pi-radiogadget-setup.sh"
install -m 755 "$HERE/dist/pi-radiogadget-radio.sh" "$PREFIX/pi-radiogadget-radio.sh"
install -m 755 "$HERE/dist/pi-radiogadget-update.sh" "$PREFIX/pi-radiogadget-update.sh"
[ -f "$HERE/README.md" ] && install -m 644 "$HERE/README.md" "$PREFIX/README.md"
install -m 644 "$HERE/dist/pi-radiogadget.service" /etc/systemd/system/pi-radiogadget.service
[ -f /etc/default/pi-radiogadget ] || install -m 644 "$HERE/dist/pi-radiogadget.default" /etc/default/pi-radiogadget
systemctl daemon-reload
echo "installed to $PREFIX"

if [ "${1:-}" = "--enable" ]; then
    # The earlier prototype services use the same USB controller.
    systemctl disable btgadget.service wifigadget.service 2>/dev/null || true
    systemctl enable pi-radiogadget.service
    echo "enabled: pi-radiogadget starts at boot. Start it now with: systemctl start pi-radiogadget"
    echo "NOTE: while it runs, the Pi's own WiFi belongs to the PC (no SSH over WiFi)."
else
    echo "service not enabled. Try it with: systemctl start pi-radiogadget   (stop: systemctl stop pi-radiogadget)"
    echo "Make it permanent with: $0 --enable"
fi
