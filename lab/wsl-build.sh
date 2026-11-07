#!/usr/bin/env bash
# Lab builds in our own WSL distro (renneros-build, Arch), not in a QEMU VM.
#
#   wsl.exe -d renneros-build -- bash "/mnt/e/Development/renner.dev OS/lab/wsl-build.sh" [installer] [iso]
#
#   installer  test installer-core, build renner-installer (kms GUI), publish it to the lab artifacts
#   iso        patch the newest official ISO with that installer (lab options: cache mirror,
#              serial console) into artifacts/renner-os-lab.iso
#
# The repo is read from /mnt/e; the cargo target dir stays on the Linux disk (fast),
# compiler output is cached by sccache under E:/sccache/renneros-wsl.
set -Eeuo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
LAB=/mnt/e/AgentData/renneros-lab
ART=$LAB/artifacts
export CARGO_TARGET_DIR=$HOME/target/renner-installer
export RUSTC_WRAPPER=sccache SCCACHE_DIR=/mnt/e/sccache/renneros-wsl
mkdir -p "$ART" "$SCCACHE_DIR"

steps=("$@")
((${#steps[@]})) || steps=(installer iso)

for step in "${steps[@]}"; do
    case $step in
        installer)
            echo "==> system/MANIFEST up to date"
            python3 "$REPO/theme/manifest.py" --check
            echo "==> package names in install.sh"
            missing=$(bash "$REPO/lab/pkgcheck.sh" "$REPO/install/install.sh" | grep MISSING || true)
            [[ -z $missing ]] || { echo "$missing" >&2; exit 1; }
            echo "==> installer-core tests"
            cargo test --manifest-path "$REPO/installer/Cargo.toml" -p installer-core --quiet
            echo "==> renner-installer (kms)"
            cargo build --manifest-path "$REPO/installer/Cargo.toml" --release -p renner-installer --features kms --quiet
            install -m 755 "$CARGO_TARGET_DIR/release/renner-installer" "$ART/renner-installer"
            (cd "$ART" && sha256sum renner-installer >renner-installer.sha256)
            echo "    $(cut -c1-16 "$ART/renner-installer.sha256")...  $ART/renner-installer"
            ;;
        iso)
            iso=$(ls "$LAB"/isos/archlinux-*-x86_64.iso | sort | tail -1)
            pacman -Sy >/dev/null   # the package lists patch-iso.sh resolves against
            echo "==> patching $(basename "$iso")"
            bash "$REPO/iso/patch-iso.sh" --iso "$iso" --installer "$ART/renner-installer" \
                --out "$HOME/renner-os-lab.iso" --mirror http://10.0.2.2:8079 --serial-console --test --repo "$LAB/repo"
            install -m 644 "$HOME/renner-os-lab.iso" "$ART/renner-os-lab.iso"
            echo "    $ART/renner-os-lab.iso"
            ;;
        *) echo "unknown step: $step" >&2; exit 2 ;;
    esac
done
