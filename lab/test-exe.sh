#!/usr/bin/env bash
# Lab: run a Windows installer in an installed, logged-in VM through renner-exe
# (as a double-click would) and pull its log to the host every 15 s.
#   VM=smoke lab/test-exe.sh [URL]       default: the Battle.net installer
set -u
cd "$(dirname "$0")/.."
VM=${VM:-smoke}
URL=${1:-https://downloader.battle.net/download/getInstaller?os=win&installer=Battle.net-Setup.exe}
L() { python lab/lab.py "$@"; }
S=E:/AgentData/renneros-lab/vms/$VM/shots
ART=E:/AgentData/renneros-lab/artifacts
rm -f $ART/exe-*.log
L key $VM alt+f2; sleep 2; L type $VM konsole --enter; sleep 6
L type $VM "cd ~/Downloads && curl -fsSL -o test.exe '$URL' && (while sleep 15; do for f in ~/.local/state/renner-os/exe/*.log; do curl -s -T \"\$f\" http://10.0.2.2:8079/_lab/artifacts/exe-\$(basename \"\$f\"); done; done &) ; time renner-exe test.exe" --enter
for i in $(seq 1 40); do
  sleep 15
  L shot $VM $S/E-$i.png >/dev/null 2>&1
  ls $ART/exe-*.log >/dev/null 2>&1 && grep -q "== exit" $ART/exe-*.log && break
done
L shot $VM $S/E-last.png
tail -c 3000 $ART/exe-*.log 2>/dev/null
echo test-exe-done
