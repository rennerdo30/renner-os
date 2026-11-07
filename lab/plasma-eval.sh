#!/usr/bin/env bash
# Lab: run a Plasma shell script (theme/*.js plus a call) in the logged-in
# session of a VM, then restart plasmashell and take a screenshot.
#   VM=smoke lab/plasma-eval.sh 'panels().forEach(p => p.widgets("org.kde.plasma.systemtray").forEach(rennerSystray));' [shot]
set -euo pipefail
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
VM=${VM:-smoke}
USER_NAME=${USER_NAME:-tom}
L() { python lab/lab.py "$@"; }
X() { L exec $VM --timeout "${T:-60}" -- "$@"; }
ART=E:/AgentData/renneros-lab/artifacts
{ cat theme/*.js; echo; echo "$1"; } >"$ART/plasma-eval.js"
uid=$(X id -u "$USER_NAME")
AS=(runuser -u "$USER_NAME" -- env "XDG_RUNTIME_DIR=/run/user/$uid" "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$uid/bus")
X "${AS[@]}" sh -c '
    curl -fsS -o /tmp/plasma-eval.js http://10.0.2.2:8079/artifacts/plasma-eval.js
    dbus-send --session --print-reply --dest=org.kde.plasmashell /PlasmaShell \
        org.kde.PlasmaShell.evaluateScript string:"$(cat /tmp/plasma-eval.js)"
    rm -f /tmp/plasma-eval.js
    systemctl --user restart plasma-plasmashell.service'
sleep 7
L shot $VM "E:/AgentData/renneros-lab/vms/$VM/shots/P-${2:-eval}.png"
