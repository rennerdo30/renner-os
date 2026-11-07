#!/usr/bin/env bash
# renner.dev OS installer. Runs on the official Arch Linux ISO (UEFI only).
#
#   curl -fsSL <source>/install/install.sh | bash
#
# Disk layout (GPT):
#   1  EFI system partition, 2 GiB, FAT32, mounted at /efi
#   2  btrfs, label "renneros", subvolumes:
#        @deployments/<id>  read-only system, one per version (booted one = /)
#        @home              /home
#        @var               /var  (shared by all deployments)
#        @etc-overlay       writable layer over the read-only /etc
# Boot: systemd-boot with one unified kernel image per deployment.
#
# Options (anything left out is asked interactively):
#   --disk DEV  --hostname NAME  --user NAME  --password PW (testing only)
#   --timezone ZONE (UTC)  --locale LOCALE (en_US.UTF-8)  --keymap MAP (us)
#   --source URL     where install/ and system/ are fetched from
#   --serial-console add console=ttyS0 (test VMs)
#   --yes            do not ask before erasing the disk
set -Eeuo pipefail

SOURCE=${RENNER_SOURCE:-https://raw.githubusercontent.com/rennerdo30/renner-os/main}
DISK="" HOSTNAME_="" USERNAME="" PASSWORD="" TIMEZONE=UTC LOCALE=en_US.UTF-8 KEYMAP=us
SERIAL=0 ASSUME_YES=0
LOG=/tmp/renner-install.log TEE_PID=""
TARGET=/mnt
STAGING=/tmp/renner-install

SYSTEM_FILES=(
    usr/bin/renner-update
    usr/lib/renner-os/initrd-etc-overlay
    usr/lib/systemd/system/renner-etc-overlay.service
    usr/lib/initcpio/install/renner-etc-overlay
)

PACKAGES=(
    base linux linux-firmware amd-ucode intel-ucode
    btrfs-progs dosfstools arch-install-scripts systemd-ukify
    networkmanager sudo nano less man-db
)

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\n\033[31mrenner-install: %s\033[0m\nLog: %s\n' "$*" "$LOG" >&2; flush_log; exit 1; }
ask() {  # ask VAR "prompt" [default] -- reads from the terminal, also under curl | bash
    local reply
    read -r -p "$2${3:+ [$3]}: " reply </dev/tty
    printf -v "$1" '%s' "${reply:-${3:-}}"
}

parse_args() {
    while (($#)); do
        case $1 in
            --disk) DISK=$2; shift ;;
            --hostname) HOSTNAME_=$2; shift ;;
            --user) USERNAME=$2; shift ;;
            --password) PASSWORD=$2; shift ;;
            --timezone) TIMEZONE=$2; shift ;;
            --locale) LOCALE=$2; shift ;;
            --keymap) KEYMAP=$2; shift ;;
            --source) SOURCE=$2; shift ;;
            --serial-console) SERIAL=1 ;;
            --yes) ASSUME_YES=1 ;;
            *) die "unknown option: $1" ;;
        esac
        shift
    done
}

check_environment() {
    say "Checking the environment"
    [[ $EUID -eq 0 ]] || die "run as root (the Arch ISO logs you in as root)"
    [[ -d /sys/firmware/efi/efivars ]] || die "UEFI boot required; this machine booted in BIOS mode"
    [[ -d /run/archiso ]] || die "run this from the official Arch Linux ISO"
    curl -fsS --max-time 15 -o /dev/null "$SOURCE/install/install.sh" \
        || die "cannot reach $SOURCE (network?)"
}

collect_settings() {
    if [[ -z $DISK ]]; then
        lsblk -dpno NAME,SIZE,MODEL -e 7,11
        ask DISK "Disk to erase and install on"
    fi
    [[ -b $DISK ]] || die "$DISK is not a block device"
    # Resolve aliases (/dev/disk/by-id/...) to the kernel name, so partition
    # names come out right, and refuse partitions or other non-disk devices.
    DISK=$(realpath -e "$DISK")
    [[ $(lsblk -dno TYPE "$DISK") == disk ]] || die "$DISK is not a whole disk (pick e.g. /dev/nvme0n1, not a partition)"
    if lsblk -nlo MOUNTPOINTS "$DISK" | grep -q .; then
        die "$DISK has mounted partitions; unmount them first"
    fi
    [[ -n $HOSTNAME_ ]] || ask HOSTNAME_ "Computer name" renner
    [[ -n $USERNAME ]] || ask USERNAME "User name"
    [[ $USERNAME =~ ^[a-z_][a-z0-9_-]*$ ]] || die "invalid user name '$USERNAME'"
    if [[ -z $PASSWORD ]]; then
        local again
        read -r -s -p "Password for $USERNAME: " PASSWORD </dev/tty; echo
        read -r -s -p "Repeat password: " again </dev/tty; echo
        [[ -n $PASSWORD && $PASSWORD == "$again" ]] || die "passwords are empty or do not match"
    fi
    [[ -e /usr/share/zoneinfo/$TIMEZONE ]] || die "unknown timezone '$TIMEZONE'"

    say "Summary"
    echo "  Disk:     $DISK ($(lsblk -dno SIZE "$DISK")), ALL DATA ON IT WILL BE ERASED"
    echo "  Computer: $HOSTNAME_   User: $USERNAME"
    echo "  Timezone: $TIMEZONE   Locale: $LOCALE   Keymap: $KEYMAP"
    if ((!ASSUME_YES)); then
        local answer
        ask answer "Type the disk name ($DISK) to erase it and install"
        [[ $answer == "$DISK" ]] || die "cancelled; nothing was changed"
    fi
}

fetch_system_files() {
    say "Fetching renner.dev OS files from $SOURCE"
    rm -rf "$STAGING"
    local f
    for f in "${SYSTEM_FILES[@]}"; do
        mkdir -p "$STAGING/$(dirname "$f")"
        curl -fsSL --retry 2 -o "$STAGING/$f" "$SOURCE/system/$f" || die "download failed: $f"
    done
}

part() {  # part DISK N -> partition device, as the kernel names it
    local dev
    dev=$(lsblk -lnpo NAME,PARTN "$1" | awk -v n="$2" '$2 == n { print $1 }')
    [[ -b $dev ]] || die "partition $2 of $1 not found"
    echo "$dev"
}

partition_disk() {
    say "Partitioning $DISK"
    umount -R "$TARGET" 2>/dev/null || true
    wipefs -af "$DISK" >/dev/null
    sgdisk -Z "$DISK" >/dev/null
    sgdisk -n1:0:+2G -t1:ef00 -c1:EFI -n2:0:0 -t2:8300 -c2:renneros "$DISK" >/dev/null
    partprobe "$DISK"
    udevadm settle
    ESP_DEV=$(part "$DISK" 1)
    ROOT_DEV=$(part "$DISK" 2)
    mkfs.fat -F 32 -n EFI "$ESP_DEV" >/dev/null
    mkfs.btrfs -f -L renneros "$ROOT_DEV" >/dev/null
    udevadm settle
    ROOT_UUID=$(blkid -s UUID -o value "$ROOT_DEV")
    ESP_UUID=$(blkid -s UUID -o value "$ESP_DEV")
}

create_subvolumes() {
    say "Creating btrfs subvolumes"
    DEPLOYMENT=$(date +%Y%m%d%H%M%S)
    mount "$ROOT_DEV" "$TARGET"
    btrfs subvolume create "$TARGET"/{@deployments,@home,@var,@etc-overlay} >/dev/null
    btrfs subvolume create "$TARGET/@deployments/$DEPLOYMENT" >/dev/null
    umount "$TARGET"

    local opts=compress=zstd:1,noatime
    mount -o "subvol=/@deployments/$DEPLOYMENT,$opts" "$ROOT_DEV" "$TARGET"
    mkdir -p "$TARGET"/{home,var,efi}
    mount -o "subvol=/@home,$opts" "$ROOT_DEV" "$TARGET/home"
    mount -o "subvol=/@var,$opts" "$ROOT_DEV" "$TARGET/var"
    mount -o umask=0077 "$ESP_DEV" "$TARGET/efi"
}

install_packages() {
    say "Installing Arch Linux packages (this takes a while)"
    pacstrap -K "$TARGET" "${PACKAGES[@]}"

    # The package database belongs to the deployment, not to the shared /var,
    # so a rollback also rolls back what pacman thinks is installed.
    mkdir -p "$TARGET/usr/lib/sysimage"
    mv "$TARGET/var/lib/pacman" "$TARGET/usr/lib/sysimage/pacman"
    ln -s ../../usr/lib/sysimage/pacman "$TARGET/var/lib/pacman"
    sed -i 's|^#\?DBPath.*|DBPath = /usr/lib/sysimage/pacman/|' "$TARGET/etc/pacman.conf"
}

install_system_files() {
    say "Installing renner.dev OS components"
    local f mode
    for f in "${SYSTEM_FILES[@]}"; do
        mode=644
        [[ $f == usr/bin/* || $f == usr/lib/renner-os/* ]] && mode=755
        install -Dm"$mode" "$STAGING/$f" "$TARGET/$f"
    done
}

configure_system() {
    say "Configuring the system"
    cat >"$TARGET/etc/fstab" <<EOF
# renner.dev OS. / is the read-only deployment chosen at boot (kernel command line).
UUID=$ROOT_UUID  /home  btrfs  subvol=/@home,compress=zstd:1,noatime  0 0
UUID=$ROOT_UUID  /var   btrfs  subvol=/@var,compress=zstd:1,noatime   0 0
UUID=$ESP_UUID   /efi   vfat   umask=0077                              0 2
# Writable places that live on /var
/var/roothome    /root       none  bind  0 0
/var/usrlocal    /usr/local  none  bind  0 0
EOF
    install -d -m 700 "$TARGET/var/roothome"
    install -d -m 755 "$TARGET/var/usrlocal"
    cp -a "$TARGET/usr/local/." "$TARGET/var/usrlocal/" 2>/dev/null || true

    echo "$HOSTNAME_" >"$TARGET/etc/hostname"
    cat >"$TARGET/etc/hosts" <<EOF
127.0.0.1  localhost
::1        localhost
127.0.1.1  $HOSTNAME_
EOF
    ln -sf "/usr/share/zoneinfo/$TIMEZONE" "$TARGET/etc/localtime"
    sed -i "s/^#\($LOCALE \)/\1/" "$TARGET/etc/locale.gen"
    echo "LANG=$LOCALE" >"$TARGET/etc/locale.conf"
    echo "KEYMAP=$KEYMAP" >"$TARGET/etc/vconsole.conf"
    arch-chroot "$TARGET" locale-gen >/dev/null
    arch-chroot "$TARGET" hwclock --systohc

    arch-chroot "$TARGET" useradd -m -G wheel -s /bin/bash "$USERNAME"
    printf '%s:%s\n' "$USERNAME" "$PASSWORD" | arch-chroot "$TARGET" chpasswd
    echo '%wheel ALL=(ALL:ALL) ALL' >"$TARGET/etc/sudoers.d/10-wheel"
    chmod 440 "$TARGET/etc/sudoers.d/10-wheel"

    # systemd-boot-check-no-failures: a new deployment is only marked good
    # (boot counting) when no unit failed, not merely when sysinit was reached.
    arch-chroot "$TARGET" systemctl enable NetworkManager.service systemd-timesyncd.service \
        systemd-boot-update.service systemd-boot-check-no-failures.service >/dev/null
}

setup_boot() {
    say "Setting up the boot loader"
    local console=""
    ((SERIAL)) && console=" console=tty0 console=ttyS0,115200"
    install -d "$TARGET/etc/kernel"
    echo "root=UUID=$ROOT_UUID rootfstype=btrfs rootflags=subvol=/@deployments/$DEPLOYMENT,compress=zstd:1,noatime ro$console" \
        >"$TARGET/etc/kernel/cmdline"

    sed -i 's/^HOOKS=.*/HOOKS=(base systemd autodetect microcode modconf kms keyboard sd-vconsole block filesystems renner-etc-overlay fsck)/' \
        "$TARGET/etc/mkinitcpio.conf"
    cat >"$TARGET/etc/mkinitcpio.d/linux.preset" <<'EOF'
# renner.dev OS: one unified kernel image per deployment, copied to the ESP
# by the installer and renner-update.
ALL_kver="/boot/vmlinuz-linux"
PRESETS=('default')
default_uki="/boot/renner.efi"
default_options="--cmdline /etc/kernel/cmdline"
EOF
    rm -f "$TARGET"/boot/initramfs-linux*.img
    arch-chroot "$TARGET" mkinitcpio -P
    [[ -s $TARGET/boot/renner.efi ]] || die "the kernel image was not built"

    arch-chroot "$TARGET" bootctl install --esp-path=/efi >/dev/null
    cat >"$TARGET/efi/loader/loader.conf" <<'EOF'
default renner-*
timeout 3
editor no
EOF
    install -d "$TARGET/efi/EFI/Linux"
    cp "$TARGET/boot/renner.efi" "$TARGET/efi/EFI/Linux/renner-$DEPLOYMENT.efi"
}

finish() {
    say "Finishing"
    cp "$LOG" "$TARGET/var/log/renner-install.log"
    sync
    btrfs property set -ts "$TARGET" ro true
    umount -R "$TARGET"
    say "renner.dev OS is installed. Remove the install medium and reboot."
    flush_log
}

flush_log() {  # wait until tee has written everything (it runs asynchronously)
    [[ -n $TEE_PID ]] || return 0
    exec 1>&- 2>&-
    wait "$TEE_PID" 2>/dev/null || true
}

main() {
    parse_args "$@"
    # Open the log before anything else; without it nothing destructive runs.
    : >>"$LOG" || { echo "renner-install: cannot write $LOG" >&2; exit 1; }
    exec > >(tee -a "$LOG") 2>&1
    TEE_PID=$!
    trap 'die "failed at line $LINENO"' ERR
    check_environment
    collect_settings
    fetch_system_files
    partition_disk
    create_subvolumes
    install_packages
    install_system_files
    configure_system
    setup_boot
    finish
}

main "$@"
