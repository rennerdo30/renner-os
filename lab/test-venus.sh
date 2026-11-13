#!/usr/bin/env bash
# Lab: Vulkan (Venus) smoke test with a Venus-capable QEMU build. Starts the VM,
# logs in through the guest agent, runs vkcube for SECS seconds in the desktop
# session and reports whether QEMU stayed responsive.
#   QEMU_DIR=E:/AgentData/winq-build/bin VM=smoke SECS=10 lab/test-venus.sh
# Logs: QEMU guest errors and virglrenderer go to the VM folder (qemu.log,
# virgl.log).
set -u
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
VM=${VM:-smoke}
SECS=${SECS:-10}
QEMU_DIR=${QEMU_DIR:?folder of a Venus-capable QEMU build}
LAB=${LAB:-E:/AgentData/renneros-lab}
L() { python lab/lab.py "$@"; }
X() { L exec $VM --timeout "${T:-60}" -- "$@"; }
fail() { echo "FAILED: $*"; exit 1; }

if ! L status | grep -q "$VM: stopped"; then
    X systemctl poweroff >/dev/null 2>&1
    for i in $(seq 1 40); do L status | grep -q "$VM: stopped" && break; sleep 2; done
fi
rm -f "$LAB/vms/$VM/virgl.log"
VIRGL_LOG_LEVEL=info VIRGL_LOG_FILE="$LAB/vms/$VM/virgl.log" \
    L up $VM --window --venus --qemu "$QEMU_DIR" --qemu-args "-d guest_errors" --minutes 60 --no-cache \
    >/dev/null || fail "VM did not start"
VM=$VM bash lab/login.sh >/dev/null || fail "login"
X sh -c 'vulkaninfo --summary 2>&1 | grep -m1 deviceName'
T=$((SECS + 30)) X sh -c "u=\$(id -u tom); w=\$(ls /run/user/\$u | grep -m1 '^wayland-[0-9]\$');
    runuser -u tom -- env XDG_RUNTIME_DIR=/run/user/\$u WAYLAND_DISPLAY=\$w sh -c 'cd /tmp; timeout $SECS vkcube --wsi wayland >/tmp/vk.log 2>&1; echo vkcube-rc=\$?'"
if X true >/dev/null 2>&1; then
    echo "qemu: responsive"
else
    echo "qemu: HUNG (guest agent does not answer)"
fi
echo "--- qemu.log"
grep -v -- "-name renneros\|interrupt vector\|register 00002095" "$LAB/vms/$VM/qemu.log" | tail -8
echo "--- virgl.log"
grep -v "gl_version\|in-process thread" "$LAB/vms/$VM/virgl.log" 2>/dev/null | tail -12
echo "--- guest"
X sh -c 'journalctl -b -o cat --no-pager | grep -E "virtio_gpu_dequeue|vkcube.*segfault" | tail -4' 2>/dev/null
