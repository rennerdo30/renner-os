#!/usr/bin/env bash
# renner.dev OS bootstrap. Run it on the official Arch Linux ISO:
#
#   curl -fsSL <base>/install/bootstrap.sh | bash                    graphical installer
#   curl -fsSL <base>/install/bootstrap.sh | bash -s -- --config FILE-or-URL [--check]
#
# It prepares the live system for renner-installer (the three libraries the
# installer's display and input need, added to the ISO's RAM only), downloads
# the installer, checks it against its published SHA256, and starts it.
# Everything after --  is passed to renner-installer.
#
# Environment (lab and testing):
#   RENNER_INSTALLER_URL  where the installer binary is (a .sha256 must sit next to it)
#   RENNER_SOURCE         passed on: where install/ and system/ come from
set -Eeuo pipefail

INSTALLER_URL=${RENNER_INSTALLER_URL:-}
BIN=/usr/local/bin/renner-installer

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\033[31mrenner-bootstrap: %s\033[0m\n' "$*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || die "run as root (the Arch ISO logs you in as root)"
[[ -d /run/archiso ]] || die "run this from the official Arch Linux ISO"
[[ -n $INSTALLER_URL ]] || die "no installer published yet; set RENNER_INSTALLER_URL (see RO-14)"

say "Preparing the live system"
systemctl start pacman-init.service || die "the ISO's package keyring could not be set up"
pacman -Sy --noconfirm --needed libinput libxkbcommon fontconfig >/dev/null \
    || die "could not add the installer's libraries to the live system"

say "Downloading renner-installer"
curl -fsSL --retry 2 -o "$BIN.part" "$INSTALLER_URL" || die "download failed: $INSTALLER_URL"
expected=$(curl -fsSL --retry 2 "$INSTALLER_URL.sha256" | cut -d' ' -f1) \
    || die "no checksum at $INSTALLER_URL.sha256"
actual=$(sha256sum "$BIN.part" | cut -d' ' -f1)
[[ -n $expected && $actual == "$expected" ]] || { rm -f "$BIN.part"; die "checksum mismatch; not running it"; }
chmod 755 "$BIN.part"
mv "$BIN.part" "$BIN"

# A config given as a URL is fetched first, so the installer only sees files.
args=()
while (($#)); do
    if [[ $1 == --config && ${2:-} == http*://* ]]; then
        curl -fsSL --retry 2 -o /tmp/renner-install.toml "$2" || die "could not fetch $2"
        args+=(--config /tmp/renner-install.toml)
        shift 2
    else
        args+=("$1")
        shift
    fi
done

say "Starting renner-installer"
exec "$BIN" "${args[@]}" </dev/tty
