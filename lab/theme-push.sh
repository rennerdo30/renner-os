#!/usr/bin/env bash
# Lab: fast theme iteration on an installed VM, no reinstall (owner,
# 2026-11-08). Regenerates the theme files, copies everything theme-related
# from system/ into the running VM's deployment (unlocked only for the copy),
# reloads Plasma and KWin and takes a screenshot.
#   VM=smoke lab/theme-push.sh [shot-name]
# Changes made this way are not a release: a full cycle (lab/cycle.sh) still
# confirms them on a fresh install.
set -euo pipefail
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
VM=${VM:-smoke}
USER_NAME=${USER_NAME:-tom}
L() { python lab/lab.py "$@"; }
X() { L exec $VM --timeout "${T:-60}" -- "$@"; }
ART=E:/AgentData/renneros-lab/artifacts

python theme/colors.py >/dev/null
python theme/plasma_style.py >/dev/null
python theme/layout_sync.py >/dev/null
python theme/manifest.py >/dev/null
# Theme-related parts of system/ (the rest needs a real update).
tar --force-local -C system -cf "$ART/theme.tar" \
    usr/share/plasma usr/share/color-schemes usr/share/icons usr/share/wallpapers \
    usr/share/renner-os/xdg usr/share/kxmlgui5 usr/lib/renner-os/trim-themes \
    usr/share/libalpm/hooks/90-renner-trim-themes.hook
# Only /usr: /etc is an overlay whose lower layer must not change while it is
# mounted (writing there broke every app's colors in the lab, 2026-11-08).
python lab/lab.py cache status | grep -q "running pid" || python lab/lab.py cache up --minutes 120 >/dev/null

uid=$(X id -u "$USER_NAME")
AS=(runuser -u "$USER_NAME" -- env "XDG_RUNTIME_DIR=/run/user/$uid" "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$uid/bus")
T=120 X sh -c 'set -e
    curl -fsS -o /tmp/theme.tar http://10.0.2.2:8079/artifacts/theme.tar
    # The booted deployment, mounted a second time (writable) under /run: no
    # remount of / needed. Its read-only flag is set again whatever happens.
    dep=$(grep -o "subvol=/\?@deployments/[0-9]*" /proc/cmdline | cut -d= -f2)
    dev=$(findmnt -no SOURCE / | sed "s/\[.*//")
    mkdir -p /run/renner-push
    mount -o "subvol=$dep,compress=zstd:1,noatime" "$dev" /run/renner-push
    # sync before locking: btrfs writes larger files lazily, and setting the
    # read-only flag first left the 230 KB wallpaper empty (0 bytes).
    trap "sync -f /run/renner-push; btrfs filesystem sync /run/renner-push; btrfs property set -ts /run/renner-push ro true; umount /run/renner-push" EXIT
    btrfs property set -ts /run/renner-push ro false
    tar -xf /tmp/theme.tar -C /run/renner-push --no-same-owner
    /usr/lib/renner-os/trim-themes /run/renner-push
    rm -f /tmp/theme.tar'
# Reload what Plasma and KWin read; the user's own settings are reset to the
# system defaults so the pushed defaults show.
X "${AS[@]}" sh -c '
    rm -f ~/.config/kdeglobals ~/.config/kwinrc ~/.config/plasmarc ~/.config/darklyrc ~/.config/breezerc \
          ~/.config/dolphinrc ~/.local/share/kxmlgui5/dolphin/dolphinui.rc ~/.local/state/dolphinstaterc
    # The global theme defaults copied at login (first in XDG_CONFIG_DIRS);
    # Plasma writes them again from the pushed theme at the next login.
    rm -rf ~/.config/kdedefaults
    kbuildsycoca6 >/dev/null 2>&1
    dbus-send --session --dest=org.kde.KWin /KWin org.kde.KWin.reconfigure >/dev/null 2>&1 || true
    plasma-apply-colorscheme RennerDark >/dev/null 2>&1 || true
    plasma-apply-desktoptheme renner >/dev/null 2>&1 || true
    systemctl --user reset-failed plasma-plasmashell.service 2>/dev/null
    systemctl --user restart plasma-plasmashell.service'
sleep 6
shot=E:/AgentData/renneros-lab/vms/$VM/shots/T-${1:-push}.png
mkdir -p "$(dirname "$shot")"
# QEMU's screendump has no surface with --gl/--venus: take it in the guest.
L shot $VM "$shot" 2>/dev/null || VM=$VM bash lab/session-shot.sh "$shot"
