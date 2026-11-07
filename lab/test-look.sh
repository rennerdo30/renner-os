#!/usr/bin/env bash
# Lab: opens typical windows in the logged-in session (through the guest agent)
# and screenshots them, so a broken look (e.g. white apps) is seen after every
# install: the start menu, Dolphin, System Settings.
#   VM=smoke lab/test-look.sh
set -u
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
VM=${VM:-smoke}
USER_NAME=${USER_NAME:-tom}
L() { python lab/lab.py "$@"; }
S=E:/AgentData/renneros-lab/vms/$VM/shots
uid=$(L exec $VM -- id -u "$USER_NAME") || { echo "FAILED: guest agent"; exit 1; }
uid=${uid//$'\r'/}
AS=(runuser -u "$USER_NAME" -- env "XDG_RUNTIME_DIR=/run/user/$uid" "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$uid/bus")
open() {  # open NAME COMMAND...: start it in the session, screenshot, close it
    local name=$1; shift
    L exec $VM -- "${AS[@]}" systemd-run --user --collect --unit="look-$name" "$@" >/dev/null || { echo "FAILED: start $name"; return 1; }
    sleep 6
    L shot $VM "$S/L-$name.png" >/dev/null && echo "$S/L-$name.png"
    L exec $VM -- "${AS[@]}" systemctl --user stop "look-$name.service" >/dev/null 2>&1
    sleep 1
}
L exec $VM -- "${AS[@]}" dbus-send --session --dest=org.kde.plasmashell /PlasmaShell org.kde.PlasmaShell.activateLauncherMenu
sleep 2; L shot $VM "$S/L-start.png" >/dev/null && echo "$S/L-start.png"
L key $VM esc >/dev/null; sleep 1
open dolphin dolphin
open settings systemsettings
echo test-look-done
