#!/usr/bin/env bash
# Lab: screenshot of the desktop session, taken inside the guest with Spectacle
# and fetched through the guest agent. Works with --gl/--venus VMs, where QEMU's
# own screendump has no surface.
#   VM=smoke lab/session-shot.sh OUT.png [--window]   (--window: active window only)
set -u
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
VM=${VM:-smoke}
OUT=$1
MODE=-f
[[ ${2:-} == --window ]] && MODE=-a
L() { python lab/lab.py "$@"; }
L exec $VM --timeout 40 -- sh -c "u=\$(id -u tom); rm -f /tmp/lab-shot.png; runuser -u tom -- env XDG_RUNTIME_DIR=/run/user/\$u DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/\$u/bus systemd-run --user --wait --collect -p RuntimeMaxSec=30 spectacle -b -n $MODE -o /tmp/lab-shot.png >/dev/null 2>&1; test -s /tmp/lab-shot.png" \
    || { echo "FAILED: spectacle took no screenshot"; exit 1; }
L exec $VM --timeout 60 -- base64 -w0 /tmp/lab-shot.png | tr -d '\r\n' | base64 -d > "$OUT" \
    || { echo "FAILED: fetching the screenshot"; exit 1; }
echo "$OUT"
