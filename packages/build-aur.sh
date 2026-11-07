#!/usr/bin/env bash
# Builds AUR packages that renner.dev OS ships, in the lab's WSL distro, into
# the local package folder the ISO patcher puts on the ISO (renner/repo).
#   wsl.exe -d renneros-build -- bash packages/build-aur.sh darkly [more...]
# Until the signed renner-os repository exists (RO-14) this is the only source
# of these packages.
set -euo pipefail
OUT=/mnt/e/AgentData/renneros-lab/repo
mkdir -p "$OUT"
id builder >/dev/null 2>&1 || useradd -m builder
pacman -Sy --noconfirm --needed base-devel git >/dev/null
for pkg in "$@"; do
    echo "==> $pkg"
    dir=/home/builder/aur/$pkg
    rm -rf "$dir"
    runuser -u builder -- git clone -q "https://aur.archlinux.org/$pkg.git" "$dir"
    # Build dependencies from the repos, as root (makepkg -s would need sudo).
    deps=$(cd "$dir" && runuser -u builder -- bash -c 'source PKGBUILD; printf "%s\n" "${depends[@]}" "${makedepends[@]}"' | sed 's/[<>=].*//' | sort -u)
    pacman -S --noconfirm --needed $deps >/dev/null
    (cd "$dir" && runuser -u builder -- makepkg -f --noconfirm >/dev/null)
    cp "$dir"/*.pkg.tar.zst "$OUT/"
done
ls -la "$OUT"
