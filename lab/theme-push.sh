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
python theme/manifest.py >/dev/null
# Theme-related parts of system/ (the rest needs a real update).
tar -C system -cf "$ART/theme.tar" \
    usr/share/plasma usr/share/color-schemes usr/share/icons usr/share/wallpapers \
    etc/xdg/kdeglobals etc/xdg/kwinrc etc/xdg/breezerc etc/xdg/darklyrc etc/xdg/plasmarc \
    etc/xdg/ksplashrc etc/xdg/kscreenlockerrc
python lab/lab.py cache status | grep -q "running pid" || python lab/lab.py cache up --minutes 120 >/dev/null

uid=$(X id -u "$USER_NAME")
AS=(runuser -u "$USER_NAME" -- env "XDG_RUNTIME_DIR=/run/user/$uid" "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$uid/bus")
T=120 X sh -c 'set -e
    curl -fsS -o /tmp/theme.tar http://10.0.2.2:8079/artifacts/theme.tar
    btrfs property set -ts / ro false
    tar -xf /tmp/theme.tar -C / --no-same-owner
    /usr/lib/renner-os/trim-themes
    btrfs property set -ts / ro true
    rm -f /tmp/theme.tar'
# Reload what Plasma and KWin read; the user's own settings are reset to the
# system defaults so the pushed defaults show.
X "${AS[@]}" sh -c '
    rm -f ~/.config/kdeglobals ~/.config/kwinrc ~/.config/plasmarc ~/.config/darklyrc ~/.config/breezerc
    kbuildsycoca6 >/dev/null 2>&1
    dbus-send --session --dest=org.kde.KWin /KWin org.kde.KWin.reconfigure >/dev/null 2>&1 || true
    plasma-apply-colorscheme RennerDark >/dev/null 2>&1 || true
    plasma-apply-desktoptheme renner >/dev/null 2>&1 || true
    systemctl --user restart plasma-plasmashell.service'
sleep 6
shot=E:/AgentData/renneros-lab/vms/$VM/shots/T-${1:-push}.png
L shot $VM "$shot"
