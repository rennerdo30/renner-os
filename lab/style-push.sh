#!/usr/bin/env bash
# Lab: build our Qt style (Darkly fork, E:/Development/renner-style) inside the
# running VM and install it into the booted deployment, then reopen Dolphin and
# take a screenshot. No WSL involved (WSL blocks the lab's QEMU ports).
# The VM needs base-devel cmake ninja extra-cmake-modules qt6-tools.
#   VM=smoke lab/style-push.sh [shot-name]
set -euo pipefail
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
VM=${VM:-smoke}
SRC=${STYLE_SRC:-E:/Development/renner-style}
ART=E:/AgentData/renneros-lab/artifacts
L() { python lab/lab.py "$@"; }
X() { L exec $VM --timeout "${T:-60}" -- "$@"; }

tar --force-local -C "$SRC" --exclude=.git --exclude=build -cf "$ART/style.tar" .
python lab/lab.py cache status | grep -q "running pid" || python lab/lab.py cache up --minutes 120 >/dev/null

T=900 X sh -c 'set -e
    rm -rf /var/tmp/style-src; mkdir -p /var/tmp/style-src /var/tmp/style-build
    curl -fsS http://10.0.2.2:8079/artifacts/style.tar | tar -x -C /var/tmp/style-src
    cd /var/tmp/style-build
    [ -f build.ninja ] || cmake -S /var/tmp/style-src -B . -G Ninja -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_QT5=OFF -DBUILD_QT6=ON -DBUILD_TESTING=OFF >/dev/null
    ninja 2>&1 | grep -E "error|FAILED" || true
    ninja >/dev/null
    dep=$(grep -o "subvol=/\?@deployments/[0-9]*" /proc/cmdline | cut -d= -f2)
    dev=$(findmnt -no SOURCE / | sed "s/\[.*//")
    mkdir -p /run/renner-push
    mount -o "subvol=$dep,compress=zstd:1,noatime" "$dev" /run/renner-push
    trap "sync -f /run/renner-push; btrfs filesystem sync /run/renner-push; btrfs property set -ts /run/renner-push ro true; umount /run/renner-push" EXIT
    btrfs property set -ts /run/renner-push ro false
    DESTDIR=/run/renner-push ninja install >/dev/null
    echo style-installed' | tr -d '\r'

uid=$(X id -u tom | tr -d '\r')
X sh -c "pkill -u tom -x dolphin; sleep 1; runuser -u tom -- env XDG_RUNTIME_DIR=/run/user/$uid DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$uid/bus systemd-run --user --collect dolphin >/dev/null 2>&1; sleep 4" || true
shot=E:/AgentData/renneros-lab/vms/$VM/shots/S-${1:-style}.png
mkdir -p "$(dirname "$shot")"
VM=$VM bash lab/session-shot.sh "$shot" --window
