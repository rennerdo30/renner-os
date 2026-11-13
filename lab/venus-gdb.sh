#!/usr/bin/env bash
# Lab: attach gdb (MSYS2) to the running QEMU of a VM with a breakpoint script,
# start vkcube in the desktop session, and print gdb's output.
#   VM=smoke lab/venus-gdb.sh GDB_SCRIPT [SECS]
set -u
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
VM=${VM:-smoke}
SCRIPT=$1
SECS=${2:-10}
LAB=${LAB:-E:/AgentData/renneros-lab}
MSYS=${MSYS:-E:/AgentData/msys64}
OUT=$LAB/vms/$VM/gdb.out
pid=$(python -c "import json,sys;print(json.load(open(sys.argv[1]))['pid'])" "$LAB/vms/$VM/vm.json")
script_msys=$(cygpath -u "$(cygpath -w "$SCRIPT")" 2>/dev/null || echo "$SCRIPT")
MSYSTEM=UCRT64 timeout $((SECS + 60)) "$MSYS/usr/bin/bash.exe" -lc "gdb -p $pid -batch -x '$script_msys'" >"$OUT" 2>&1 &
gdb_job=$!
# gdb is ready once it has attached and set the breakpoints.
for i in $(seq 1 60); do grep -q "^Breakpoint 1 at\|^Breakpoint 1 (" "$OUT" 2>/dev/null && break; sleep 1; done
python lab/lab.py exec $VM --timeout $((SECS + 30)) -- sh -c "u=\$(id -u tom); runuser -u tom -- env XDG_RUNTIME_DIR=/run/user/\$u DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/\$u/bus systemd-run --user --wait --collect -p RuntimeMaxSec=$SECS vkcube --wsi wayland" >/dev/null 2>&1
wait $gdb_job
grep -v "^\[New Thread\|^\[Thread\|^$\|^warning: \|OBS" "$OUT"
