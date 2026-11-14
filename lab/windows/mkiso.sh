#!/bin/bash
# Builds unattend.iso (the Windows answer file as a CD) next to the Windows ISOs.
#   wsl.exe -d renneros-build -- bash lab/windows/mkiso.sh
set -e
command -v xorriso >/dev/null || pacman -S --noconfirm --needed libisoburn >/dev/null
SRC=$(cd "$(dirname "$0")" && pwd)
cd /mnt/e/AgentData/renneros-lab/win
xorriso -as mkisofs -quiet -J -r -V UNATTEND -o unattend.iso "$SRC/autounattend.xml"
ls -la unattend.iso
