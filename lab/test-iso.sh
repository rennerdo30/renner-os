#!/usr/bin/env bash
# Lab: boot the patched lab ISO (built with --test), drive the installer over
# its control channel (lab.py ui, see installer control.rs), install, restart,
# boot the new system and log in. Every step checks the installer's reply and
# waits for the page it should reach, so nothing depends on click timing.
#   VM=smoke lab/test-iso.sh       account: tom / pw1, keyboard German, Europe/Berlin
set -u
VM=${VM:-inst}
cd "$(dirname "$0")/.."
L() { python lab/lab.py "$@"; }
S=E:/AgentData/renneros-lab/vms/$VM/shots
SER=E:/AgentData/renneros-lab/vms/$VM/serial.log
ISO=E:/AgentData/renneros-lab/artifacts/renner-os-lab.iso
mkdir -p "$S"
fail() { echo "FAILED: $*"; L shot $VM $S/FAILED.png; exit 1; }
U() { L ui $VM "$@" >/dev/null || fail "ui $*"; }
page() { L ui $VM --wait-page "$1" --fail-page 7 --timeout "${2:-120}" || fail "page $1"; }

L down $VM >/dev/null; rm -f $SER
L up $VM --window --iso $ISO --minutes 60 --no-enter >/dev/null
sleep 6; L shot $VM $S/I0-bootmenu.png >/dev/null
L key $VM ret                       # WHPX: the boot menu countdown does not run
page 0 180; L shot $VM $S/I1-welcome.png >/dev/null
U pick language 2                   # Deutsch (Deutschland): keyboard de, Europe/Berlin
U next; page 1; sleep 3; L shot $VM $S/I2-network.png >/dev/null
L ui $VM state | grep -q "online=1" || fail "not online"
U next; page 2
U pick disk 0; U next; page 3
U set username tom; U set password pw1; U set password2 pw1
L shot $VM $S/I3-account.png >/dev/null
U next; page 4; L shot $VM $S/I4-summary.png >/dev/null
U next; page 5; L shot $VM $S/I5-installing.png >/dev/null
page 6 2400; L shot $VM $S/I6-done.png >/dev/null
grep -ao "RENNER-INSTALL-RC=[0-9]*" $SER
U next                               # Restart now
for i in $(seq 1 60); do L status | grep -q "$VM: stopped" && break; sleep 2; done
rm -f $SER; L up $VM --window --minutes 30 >/dev/null; sleep 40
VM=$VM bash lab/login.sh tom pw1 || fail "login as tom"
sleep 8
L shot $VM $S/I7-desktop.png
grep -ao "renner login" $SER | head -1
echo test-iso-done
