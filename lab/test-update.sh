#!/usr/bin/env bash
# Lab: the automatic update end to end, through the guest agent (RO-14).
#
# A newer renner-os package appears in the lab repository; the background
# update (renner-update.service, what the daily timer starts) builds a new
# deployment with it; the desktop shows "Update ready"; the VM restarts into
# the new deployment, which is checked (deployment, renner-os version, system
# accounts from userdb, no failed units, the update timer on, a working login).
#
# Needs a VM installed from the lab ISO (its pacman.conf points at
# http://10.0.2.2:8079/artifacts/repo) and a newer build in artifacts/repo-next
# (WSL, before the VM starts: wsl-build.sh repo-next).
#   VM=smoke lab/test-update.sh
set -u
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
VM=${VM:-smoke}
ART=E:/AgentData/renneros-lab/artifacts
SHOTS=E:/AgentData/renneros-lab/vms/$VM/shots
L() { python lab/lab.py "$@"; }
X() { L exec $VM --timeout "${T:-60}" -- "$@"; }
fail() { echo "FAILED: $*"; exit 1; }
booted() { X sh -c 'grep -o "@deployments/[0-9]*" /proc/cmdline | cut -d/ -f2' | tr -d '\r'; }
version() { X pacman -Q renner-os | tr -d '\r' | cut -d' ' -f2; }

next=$(ls "$ART"/repo-next/renner-os-*.pkg.tar.zst 2>/dev/null | head -1)
[[ -n $next ]] || fail "no newer build in $ART/repo-next (wsl-build.sh repo-next)"
next_ver=$(basename "$next" | sed -E 's/^renner-os-(.*)-any\.pkg\.tar\.zst$/\1/')
python lab/lab.py cache status | grep -q "running pid" || L cache up --minutes 120 >/dev/null

L exec $VM --wait-agent 240 -- true >/dev/null || fail "guest agent"
before=$(booted)
before_ver=$(version)
echo "booted before: $before, renner-os $before_ver"
[[ $before_ver != "$next_ver" ]] || fail "the VM already has renner-os $next_ver"
X grep -q '^Server = http://10.0.2.2:8079/artifacts/repo' /etc/pacman.conf || fail "pacman.conf does not use the lab repository"
X systemctl is-enabled -q renner-update.timer || fail "renner-update.timer is not enabled"

out=$(X renner-update check | tr -d '\r')
grep -q '^renner-os ' <<<"$out" && fail "an update is listed before the repository has one: $out"
cp -f "$ART"/repo-next/* "$ART/repo/"     # the newer build is published
echo "published renner-os $next_ver"
out=$(X renner-update check | tr -d '\r')
grep -q "^renner-os $before_ver -> $next_ver" <<<"$out" || fail "check does not list the update: $out"

VM=$VM bash lab/login.sh >/dev/null || fail "login before the update"
# The background update, as the daily timer starts it.
T=1800 X systemctl start renner-update.service >/dev/null || fail "renner-update.service: $(X journalctl -u renner-update -n 20 --no-pager)"
ready=$(X cat /run/renner-update-ready | tr -d '\r')
[[ -n $ready && $ready != "$before" ]] || fail "no update waiting for a restart"
echo "update ready: deployment $ready"
uid=$(X id -u tom | tr -d '\r')
for i in $(seq 1 15); do
    X runuser -u tom -- env XDG_RUNTIME_DIR=/run/user/$uid systemctl --user is-active -q renner-update-notify.service \
        >/dev/null 2>&1 && break
    sleep 1
done
X runuser -u tom -- env XDG_RUNTIME_DIR=/run/user/$uid systemctl --user is-active -q renner-update-notify.service \
    || fail "no update notification (renner-update-notify.service not active)"
mkdir -p "$SHOTS"
VM=$VM bash lab/session-shot.sh "$SHOTS/U-update-ready.png" >/dev/null && echo "shot: $SHOTS/U-update-ready.png"

X systemctl reboot >/dev/null 2>&1   # the lab VM powers off on reboot (-no-reboot)
for i in $(seq 1 60); do L status | grep -q "$VM: stopped" && break; sleep 2; done
L up $VM --window --minutes 60 ${LAB_UP_ARGS:-} >/dev/null || fail "VM did not start again"
L exec $VM --wait-agent 240 -- true >/dev/null || fail "guest agent after the update"
after=$(booted)
after_ver=$(version)
echo "booted after:  $after, renner-os $after_ver"
[[ $after == "$ready" ]] || fail "not on the new deployment $ready"
[[ $after_ver == "$next_ver" ]] || fail "renner-os is $after_ver, expected $next_ver"
X sh -c 'userdbctl user sddm >/dev/null && echo userdb-ok' | grep -q userdb-ok || fail "system account sddm not in userdb"
failed=$(X systemctl --failed --no-legend --plain | tr -d '\r')
[[ -z $failed ]] || fail "failed units: $failed"
X systemctl is-active -q renner-update.timer || fail "renner-update.timer is not running"
X test ! -e /run/renner-update-ready || fail "the restart left the ready marker"
VM=$VM bash lab/login.sh >/dev/null || fail "login after the update"
echo test-update-done
