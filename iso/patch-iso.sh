#!/usr/bin/env bash
# renner.dev OS ISO patcher (RO-12).
#
# Takes the official Arch Linux ISO and writes a copy that boots straight into
# the renner.dev OS installer. Arch's own files stay byte for byte as they are;
# the patch only adds:
#   /renner/                    the installer, autostart.sh, install/ and system/,
#                               and pkgs/: the libraries the installer's screen
#                               and input need, so it starts without a network
#   a "renner.dev OS installer" boot entry, made the default, with a short
#   timeout, for UEFI (systemd-boot, in the ISO tree and in the EFI image) and
#   for BIOS (syslinux)
# The original boot setup (El Torito, MBR, GPT, the appended EFI partition) is
# replayed from the input ISO by xorriso, so the result boots like the original.
#
#   iso/patch-iso.sh --iso archlinux-YYYY.MM.DD-x86_64.iso --installer renner-installer \
#                    [--out renner-os-installer.iso] [--sha256 HEX]
#                    [--mirror URL] [--source URL] [--serial-console] [--test]
#   --test: the installer takes lab commands on the second serial port
#
# Needs: xorriso (libisoburn), mtools, coreutils, and pacman with synced
# package databases (an Arch system or the lab's WSL distro) for pkgs/.
set -Eeuo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ISO= OUT= INSTALLER= SHA= MIRROR= SOURCE= SERIAL=0 TEST=0 TIMEOUT=3

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\033[31mpatch-iso: %s\033[0m\n' "$*" >&2; exit 1; }

while (($#)); do
    case $1 in
        --iso) ISO=$2; shift ;;
        --out) OUT=$2; shift ;;
        --installer) INSTALLER=$2; shift ;;
        --sha256) SHA=$2; shift ;;
        --mirror) MIRROR=$2; shift ;;
        --source) SOURCE=$2; shift ;;
        --serial-console) SERIAL=1 ;;
        --test) TEST=1 ;;
        --timeout) TIMEOUT=$2; shift ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
    shift
done

[[ -f $ISO ]] || die "--iso: no such file: $ISO"
[[ -f $INSTALLER ]] || die "--installer: no such file: $INSTALLER"
[[ $TIMEOUT =~ ^[0-9]+$ ]] || die "--timeout takes whole seconds"
for url in "$MIRROR" "$SOURCE"; do
    [[ -z $url || $url =~ ^(https?|file)://[^[:space:]\']+$ ]] || die "not a usable URL: $url"
done
for tool in xorriso mcopy mtype sha256sum pacman curl; do
    command -v "$tool" >/dev/null || die "$tool is missing (packages: libisoburn mtools)"
done
ISO=$(realpath "$ISO")
[[ $ISO != *"'"* ]] || die "the ISO path must not contain a single quote"
OUT=${OUT:-$PWD/renner-os-installer-$(basename "$ISO" | sed -E 's/^archlinux-([0-9.]+)-.*/\1/').iso}
[[ $(realpath -m "$OUT") != "$ISO" ]] || die "--out must not overwrite the input"

if [[ -n $SHA ]]; then
    say "Checking the input ISO"
    [[ $(sha256sum "$ISO" | cut -d' ' -f1) == "${SHA,,}" ]] || die "SHA256 does not match; not the official ISO"
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# ---- read the original boot setup -------------------------------------------
say "Reading the boot setup of $(basename "$ISO")"
xorriso -indev "$ISO" -report_el_torito as_mkisofs 2>/dev/null >"$WORK/boot.opts"
grep -q -- '^-append_partition 2 ' "$WORK/boot.opts" \
    || die "no appended EFI partition; this does not look like an official Arch ISO"
uuid=$(xorriso -indev "$ISO" -osirrox on -extract /loader/entries/01-archiso-linux.conf "$WORK/orig.conf" 2>/dev/null \
    && grep -o 'archisosearchuuid=[^ ]*' "$WORK/orig.conf") || die "no archiso boot entry found"
basedir=$(grep -o 'archisobasedir=[^ ]*' "$WORK/orig.conf") || die "no archisobasedir in the boot entry"

# The EFI partition (a FAT image): its block range, in 512-byte (d) or 2048-byte (s) blocks.
range=$(sed -nE "s/^-append_partition 2 [^ ]+ --interval:local_fs:([0-9]+)([ds])-([0-9]+)[ds]::.*/\1 \2 \3/p" "$WORK/boot.opts")
read -r first unit last <<<"$range"
[[ -n ${first:-} && -n ${last:-} ]] || die "could not locate the EFI partition"
bs=512; [[ $unit == s ]] && bs=2048
dd if="$ISO" of="$WORK/efi.img" bs=$bs skip="$first" count=$((last - first + 1)) status=none
mtype -i "$WORK/efi.img" ::/loader/loader.conf >/dev/null 2>&1 || die "the EFI partition has no systemd-boot loader"

# ---- the renner payload -------------------------------------------------------
say "Adding the installer"
P=$WORK/payload/renner
mkdir -p "$P/source"
install -m 755 "$INSTALLER" "$P/renner-installer"
(cd "$P" && sha256sum renner-installer >renner-installer.sha256)
install -m 755 "$REPO/iso/autostart.sh" "$P/autostart.sh"
cp -r "$REPO/install" "$REPO/system" "$P/source/"
git -c safe.directory="$REPO" -C "$REPO" rev-parse --short HEAD >"$P/source/REVISION" 2>/dev/null || echo unknown >"$P/source/REVISION"

# The installer's libraries and everything they pull in that the ISO lacks,
# resolved against the ISO's own package list.
say "Adding the installer's libraries"
xorriso -indev "$ISO" -osirrox on -extract /arch/pkglist.x86_64.txt "$WORK/pkglist" 2>/dev/null \
    || die "the ISO has no package list (arch/pkglist.x86_64.txt)"
mkdir -p "$WORK/db" "$P/pkgs"
cp -r /var/lib/pacman/sync "$WORK/db/"
pacman -Sp --dbpath "$WORK/db" --noconfirm --print-format '%n %l' libinput libxkbcommon fontconfig \
    >"$WORK/pkgs.txt" || die "could not resolve the installer's libraries (pacman -Sy first?)"
while read -r pkg url; do
    grep -q "^$pkg " "$WORK/pkglist" && continue
    curl -fsSL --retry 2 -o "$P/pkgs/${url##*/}" "$url" || die "download failed: $url"
done <"$WORK/pkgs.txt"
echo "    $(ls "$P/pkgs" | wc -l) packages, $(du -sh "$P/pkgs" | cut -f1)"

options="$basedir $uuid copytoram=n script=/run/archiso/bootmnt/renner/autostart.sh"
[[ -n $MIRROR ]] && options+=" renner.mirror=$MIRROR"
[[ -n $SOURCE ]] && options+=" renner.source=$SOURCE"
((SERIAL)) && options+=" renner.serial=1 console=tty0 console=ttyS0,115200"
((TEST)) && options+=" renner.test=1"

# systemd-boot (UEFI): a new first entry, the default, short timeout.
cat >"$WORK/00-renner-installer.conf" <<EOF
title    renner.dev OS installer
sort-key 00-renner
linux    /arch/boot/x86_64/vmlinuz-linux
initrd   /arch/boot/x86_64/initramfs-linux.img
options  $options
EOF
xorriso -indev "$ISO" -osirrox on -extract /loader/loader.conf "$WORK/loader.orig" 2>/dev/null
{
    grep -vE '^(timeout|default)\b' "$WORK/loader.orig"
    echo "timeout $TIMEOUT"
    echo "default 00-renner-installer.conf"
} >"$WORK/loader.conf"

# syslinux (BIOS): the same entry first, made the default.
xorriso -indev "$ISO" -osirrox on -extract /boot/syslinux/archiso_sys-linux.cfg "$WORK/sys-linux.orig" \
    -extract /boot/syslinux/archiso_sys.cfg "$WORK/sys.orig" 2>/dev/null
{
    cat <<EOF
LABEL renner
TEXT HELP
Install renner.dev OS.
ENDTEXT
MENU LABEL renner.dev OS installer
LINUX /arch/boot/x86_64/vmlinuz-linux
INITRD /arch/boot/x86_64/initramfs-linux.img
APPEND $options

EOF
    cat "$WORK/sys-linux.orig"
} >"$WORK/sys-linux.cfg"
sed -E "s/^DEFAULT .*/DEFAULT renner/; s/^TIMEOUT .*/TIMEOUT $((TIMEOUT * 10))/" "$WORK/sys.orig" >"$WORK/sys.cfg"
grep -q '^DEFAULT renner' "$WORK/sys.cfg" || die "syslinux layout changed; archiso_sys.cfg has no DEFAULT line"

# The EFI image gets the same entry and loader.conf.
mcopy -o -i "$WORK/efi.img" "$WORK/loader.conf" ::/loader/loader.conf
mcopy -o -i "$WORK/efi.img" "$WORK/00-renner-installer.conf" ::/loader/entries/00-renner-installer.conf

# ---- write the new ISO --------------------------------------------------------
# The replayed options, with the EFI partition taken from our patched image.
# xorriso refers to an appended partition by number, so the boot catalog
# follows it wherever it ends up.
sed -E \
    -e "s#^(-append_partition 2 [^ ]+ ).*#\1'$WORK/efi.img'#" \
    -e "s#^-e '--interval:appended_partition_2[^']*'#-e '--interval:appended_partition_2:all::'#" \
    "$WORK/boot.opts" >"$WORK/boot.new"
grep -q "^-e '--interval:appended_partition_2:all::'" "$WORK/boot.new" \
    || die "the EFI El Torito entry is not on the appended partition; layout changed"

say "Writing $(basename "$OUT")"
rm -f "$OUT"
# boot.new holds xorriso's own shell-quoted option list.
eval "set -- $(tr '\n' ' ' <"$WORK/boot.new")"
xorriso -indev "$ISO" -outdev "$OUT" \
    -map "$P" /renner \
    -map "$WORK/loader.conf" /loader/loader.conf \
    -map "$WORK/00-renner-installer.conf" /loader/entries/00-renner-installer.conf \
    -map "$WORK/sys-linux.cfg" /boot/syslinux/archiso_sys-linux.cfg \
    -map "$WORK/sys.cfg" /boot/syslinux/archiso_sys.cfg \
    -as mkisofs "$@" -- \
    2>"$WORK/xorriso.log" || { tail -20 "$WORK/xorriso.log" >&2; die "xorriso failed"; }

(cd "$(dirname "$OUT")" && sha256sum "$(basename "$OUT")" >"$(basename "$OUT").sha256")
say "Done: $OUT ($(du -h "$OUT" | cut -f1))"
