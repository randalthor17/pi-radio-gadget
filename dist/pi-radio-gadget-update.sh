#!/bin/bash
# Applies an update dropped onto the SD card's boot partition (so a PC can update the Pi without any network):
#
#   /boot/firmware/pi-radio-gadget-update/   containing any of: pi-radio-gadget (binary), *.sh, pi-radio-gadget.service,
#                                           pi-radio-gadget.default (only installed if missing)
#
# Run at every service start (ExecStartPre, before the gadget is created). The directory is renamed to
# pi-radio-gadget-update.applied afterwards, so an update is applied exactly once.
set -u
SRC=/boot/firmware/pi-radio-gadget-update
DST=/opt/pi-radio-gadget

# Rollback for updates pushed over USB (pi-radio-gadget-flash): the daemon writes "pending" when it installs a new binary
# and deletes it after 60 s of uptime. If the new binary keeps dying before that, go back to the previous one.
if [ -f "$DST/pending" ]; then
    n=$(cat "$DST/pending" 2>/dev/null); n=${n:-0}
    case "$n" in ''|*[!0-9]*) n=0 ;; esac
    if [ "$n" -ge 2 ] && [ -f "$DST/pi-radio-gadget.prev" ]; then
        echo "the new version failed to stay up twice: rolling back to the previous one"
        mv -f "$DST/pi-radio-gadget.prev" "$DST/pi-radio-gadget"
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
        pi-radio-gadget)          install -m 755 "$f" "$DST/pi-radio-gadget" ;;
        *.sh)                    install -m 755 "$f" "$DST/$(basename "$f")" ;;
        pi-radio-gadget.service)  install -m 644 "$f" /etc/systemd/system/pi-radio-gadget.service; systemctl daemon-reload ;;
        pi-radio-gadget.default)  [ -f /etc/default/pi-radio-gadget ] || install -m 644 "$f" /etc/default/pi-radio-gadget ;;
    esac
done
rm -rf "$SRC.applied"
mv "$SRC" "$SRC.applied"
sync
exit 0
