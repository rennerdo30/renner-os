#!/usr/bin/env bash
# RO-12: boot the patched ISO; it must start the installer by itself. Then a click-only install.
# Usage: lab/test-iso.sh   (after: wsl.exe -d renneros-build -- bash lab/wsl-build.sh)
# Clicks at the 1280x800 installer layout; the account is tom / pw1.
set -u
cd "$(dirname "$0")/.."
L() { python lab/lab.py "$@"; }
S=E:/AgentData/renneros-lab/vms/inst/shots
SER=E:/AgentData/renneros-lab/vms/inst/serial.log
ISO=E:/AgentData/renneros-lab/artifacts/renner-os-lab.iso
wait_s() { local end=$((SECONDS+$1)); until [ $SECONDS -ge $end ]; do sleep 1; done; }
wait_serial() { for i in $(seq 1 "$2"); do grep -aq "$1" "$SER" 2>/dev/null && return 0; sleep 3; done; return 1; }
C() { L click inst "$1" "$2"; wait_s 1; }
L down inst >/dev/null; rm -f $SER
L up inst --window --iso $ISO --minutes 45 --no-enter >/dev/null
wait_s 6; L shot inst $S/I0-bootmenu.png
L key inst ret                        # WHPX: the boot menu countdown does not run
wait_s 45
wait_s 15; L shot inst $S/I1-autostart.png
C 1057 677; C 780 279; C 1057 677                          # welcome Next, disk card, Next
C 582 292; C 604 575; C 892 575; C 856 695                 # user: tom (on-screen keyboard)
L shot inst $S/I1b-cursor.png
C 582 349; C 964 575; C 388 575; C 316 515                 # password: pw1
C 955 349; C 964 575; C 388 575; C 316 515                 # repeat
C 928 755; wait_s 1; C 1057 677; wait_s 4                  # Done, Next -> summary
L shot inst $S/I2-summary.png
C 1025 677; wait_s 8
L shot inst $S/I3-installing.png
for i in $(seq 1 90); do
  wait_s 10
  grep -aq "RENNER-INSTALL-RC=" $SER && break
  L shot inst $S/I4-progress.png >/dev/null
  L status | grep -q "inst: stopped" && break
done
L shot inst $S/I5-end.png >/dev/null
C 1057 677                                                 # ONE click: Restart now
for i in $(seq 1 60); do L status | grep -q "inst: stopped" && break; sleep 2; done
L status | grep inst
grep -ao "RENNER-INSTALL-RC=[0-9]*" $SER
rm -f $SER; L up inst --window --minutes 15 >/dev/null; wait_s 35
L type inst pw1 --enter; wait_s 25
L shot inst $S/I6-tom-plasma.png
grep -ao "renner login" $SER | head -1
echo test-iso-done
