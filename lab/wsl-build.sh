#!/usr/bin/env bash
# Lab builds in our own WSL distro (renneros-build, Arch), not in a QEMU VM.
#
#   wsl.exe -d renneros-build -- bash "/mnt/e/Development/renner.dev OS/lab/wsl-build.sh" [repo] [installer] [iso]
#
#   repo       build and sign the renner-os package repository (packages/build-repo.sh)
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
# Own port: the default 4226 can be taken by another sccache (WSL shares the
# host network); a foreign server answered with a broken reply (2026-11-14).
export SCCACHE_SERVER_PORT=4236
sccache rustc -vV >/dev/null 2>&1 || { echo "    sccache not working; building without it"; unset RUSTC_WRAPPER; }
mkdir -p "$ART" "$SCCACHE_DIR"

steps=("$@")
((${#steps[@]})) || steps=(repo installer iso)

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
        bench)
            echo "==> renner-bench (Linux)"
            pacman -Sy --needed --noconfirm cmake ninja gcc sdl2-compat vulkan-headers vulkan-icd-loader shaderc >/dev/null
            cmake -S "$REPO/bench" -B "$HOME/build/renner-bench" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
            ninja -C "$HOME/build/renner-bench"
            install -m 755 "$HOME/build/renner-bench/renner-bench" "$ART/renner-bench"
            echo "    $ART/renner-bench"
            ;;
        repo)
            echo "==> renner-os package repository"
            bash "$REPO/packages/build-repo.sh"
            ;;
        repo-next)
            # lab/test-update.sh: the same repository with a newer renner-os
            # (next release number), published while the VM runs.
            echo "==> renner-os, next release (artifacts/repo-next)"
            rm -rf "$ART/repo-next"
            cp -r "$ART/repo" "$ART/repo-next"
            cur=$(ls "$ART"/repo/renner-os-*.pkg.tar.zst | head -1 | sed -E 's/.*renner-os-(.*)-([0-9]+)-any\.pkg\.tar\.zst$/\1 \2/')
            RENNER_REPO_OUT="$ART/repo-next" RENNER_OS_VERSION=${cur% *} RENNER_PKGREL=$((${cur#* } + 1)) \
                bash "$REPO/packages/build-repo.sh" renner-os
            ;;
        iso)
            iso=$(ls "$LAB"/isos/archlinux-*-x86_64.iso | sort | tail -1)
            pacman -Sy >/dev/null   # the package lists patch-iso.sh resolves against
            echo "==> patching $(basename "$iso")"
            bash "$REPO/iso/patch-iso.sh" --iso "$iso" --installer "$ART/renner-installer" \
                --out "$HOME/renner-os-lab.iso" --mirror http://10.0.2.2:8079 --serial-console --test \
                --repo "$ART/repo" --repo-url http://10.0.2.2:8079/artifacts/repo
            install -m 644 "$HOME/renner-os-lab.iso" "$ART/renner-os-lab.iso"
            echo "    $ART/renner-os-lab.iso"
            ;;
        *) echo "unknown step: $step" >&2; exit 2 ;;
    esac
done
