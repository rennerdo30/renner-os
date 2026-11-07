#!/usr/bin/env bash
# Lab: one transactional update on an installed VM, through the guest agent:
# renner-update installs a package into a new deployment, the VM restarts into
# it, and the result is checked (deployment, system accounts from userdb, no
# failed units, the package, a working login).
#   VM=smoke lab/test-update.sh
set -u
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
VM=${VM:-smoke}
L() { python lab/lab.py "$@"; }
X() { L exec $VM --timeout "${T:-60}" -- "$@"; }
fail() { echo "FAILED: $*"; exit 1; }
booted() { X sh -c 'grep -o "@deployments/[0-9]*" /proc/cmdline | cut -d/ -f2'; }

L exec $VM --wait-agent 240 -- true >/dev/null || fail "guest agent"
before=$(booted | tr -d '\r')
echo "booted before: $before"
T=1200 X renner-update install tree >/dev/null || fail "renner-update install tree"
echo "update done"
X systemctl reboot >/dev/null 2>&1   # the lab VM powers off on reboot (-no-reboot)
for i in $(seq 1 60); do L status | grep -q "$VM: stopped" && break; sleep 2; done
L up $VM --window --minutes 60 >/dev/null || fail "VM did not start again"
L exec $VM --wait-agent 240 -- true >/dev/null || fail "guest agent after the update"
after=$(booted | tr -d '\r')
echo "booted after:  $after"
[[ -n $after && $after != "$before" ]] || fail "still on the old deployment"
X sh -c 'userdbctl user sddm --output=short >/dev/null && echo userdb-ok' | grep -q userdb-ok || fail "system account sddm not in userdb"
failed=$(X systemctl --failed --no-legend --plain | tr -d '\r')
[[ -z $failed ]] || fail "failed units: $failed"
X sh -c 'command -v tree' >/dev/null || fail "tree is not installed"
VM=$VM bash lab/login.sh >/dev/null || fail "login after the update"
echo test-update-done
