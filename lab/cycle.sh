#!/usr/bin/env bash
# One lab cycle from this checkout: build in WSL, install the patched ISO into
# a VM, optionally run a Windows program. Stops at the first failure.
#   VM=smoke lab/cycle.sh [exe [URL]]
set -euo pipefail
cd "$(dirname "$0")/.."
VM=${VM:-smoke}
export VM
repo_wsl=/mnt/$(pwd -W 2>/dev/null | sed -E 's#^([A-Za-z]):#\L\1#' || pwd)
# QEMU keeps the ISO open; the build cannot replace it while a VM uses it.
python lab/lab.py down "$VM" >/dev/null
echo "== build ($repo_wsl)"
MSYS_NO_PATHCONV=1 wsl.exe -d renneros-build -- timeout 1800 bash "$repo_wsl/lab/wsl-build.sh" installer iso
echo "== install into $VM"
out=$(bash lab/test-iso.sh 2>&1) || true
grep -E "RENNER-INSTALL-RC|renner login|test-iso-done" <<<"$out" || true
grep -q "RENNER-INSTALL-RC=0" <<<"$out" || { echo "install failed; see $VM shots" >&2; exit 1; }
if [[ ${1:-} == exe ]]; then
    echo "== Windows program"
    bash lab/test-exe.sh "${@:2}"
fi
echo cycle-done
