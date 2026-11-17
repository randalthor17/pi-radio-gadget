#!/bin/bash
# Applies an update dropped onto the SD card's boot partition (so a PC can update the Pi without any network):
#
#   /boot/firmware/pi-radiogadget-update/   containing any of: pi-radiogadget (binary), *.sh, pi-radiogadget.service,
#                                           pi-radiogadget.default (only installed if missing)
#
# Run at every service start (ExecStartPre, before the gadget is created). The directory is renamed to
# pi-radiogadget-update.applied afterwards, so an update is applied exactly once.
set -u
SRC=/boot/firmware/pi-radiogadget-update
DST=/opt/pi-radiogadget

# Rollback for updates pushed over USB (pi-radiogadget-flash): the daemon writes "pending" when it installs a new binary
# and deletes it after 60 s of uptime. If the new binary keeps dying before that, go back to the previous one.
if [ -f "$DST/pending" ]; then
    n=$(cat "$DST/pending" 2>/dev/null); n=${n:-0}
    case "$n" in ''|*[!0-9]*) n=0 ;; esac
    if [ "$n" -ge 2 ] && [ -f "$DST/pi-radiogadget.prev" ]; then
        echo "the new version failed to stay up twice: rolling back to the previous one"
        mv -f "$DST/pi-radiogadget.prev" "$DST/pi-radiogadget"
        rm -f "$DST/pending"
    else
        echo $((n + 1)) > "$DST/pending"
    fi
fi

[ -d "$SRC" ] || exit 0
echo "applying update from $SRC"
for f in "$SRC"/*; do
    [ -f "$f" ] || continue
    case "$(basename "$f")" in
        pi-radiogadget)          install -m 755 "$f" "$DST/pi-radiogadget" ;;
        *.sh)                    install -m 755 "$f" "$DST/$(basename "$f")" ;;
        pi-radiogadget.service)  install -m 644 "$f" /etc/systemd/system/pi-radiogadget.service; systemctl daemon-reload ;;
        pi-radiogadget.default)  [ -f /etc/default/pi-radiogadget ] || install -m 644 "$f" /etc/default/pi-radiogadget ;;
    esac
done
rm -rf "$SRC.applied"
mv "$SRC" "$SRC.applied"
sync
exit 0
