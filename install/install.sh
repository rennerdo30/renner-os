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
#   --password-hash HASH  crypt(3) hash instead of a password (renner-installer)
#   --no-desktop     console system only, no Plasma or GPU drivers
#   --timezone ZONE (UTC)  --locale LOCALE (en_US.UTF-8)  --keymap MAP (us)
#   --source URL     where install/ and system/ are fetched from
#   --serial-console add console=ttyS0 (test VMs)
#   --yes            do not ask before erasing the disk
set -Eeuo pipefail

SOURCE=${RENNER_SOURCE:-https://raw.githubusercontent.com/rennerdo30/renner-os/main}
DISK="" HOSTNAME_="" USERNAME="" PASSWORD="" PASSWORD_HASH="" TIMEZONE=UTC LOCALE=en_US.UTF-8 KEYMAP=us
SERIAL=0 ASSUME_YES=0 DESKTOP=1
LOG=/tmp/renner-install.log
TARGET=/mnt
STAGING=/tmp/renner-install

SYSTEM_FILES=(
    usr/bin/renner-update
    usr/bin/renner-gpu-detect
    usr/lib/renner-os/initrd-etc-overlay
    usr/lib/systemd/system/renner-etc-overlay.service
    usr/lib/initcpio/install/renner-etc-overlay
    usr/lib/renner-os/activate
    usr/lib/renner-os/boot-health
    usr/lib/renner-os/essential.units
    usr/lib/systemd/system/renner-activate-accounts.service
    usr/lib/systemd/system/renner-activate-var.service
    usr/lib/systemd/system/renner-boot-health.service
)

PACKAGES=(
    base linux linux-firmware amd-ucode intel-ucode
    btrfs-progs dosfstools arch-install-scripts systemd-ukify
    networkmanager sudo nano less man-db
)

# KDE Plasma desktop. The look (renner theme) comes later as its own package.
DESKTOP_PACKAGES=(
    plasma-meta sddm
    konsole dolphin kate ark spectacle gwenview okular kcalc filelight partitionmanager
    ffmpegthumbs kdegraphics-thumbnailers kio-extras
    pipewire pipewire-pulse pipewire-alsa pipewire-jack wireplumber
    inter-font ttf-jetbrains-mono noto-fonts noto-fonts-emoji noto-fonts-cjk
    flatpak bluez bluez-utils firefox
)
# GPU driver packages come from renner-gpu-detect (see install_desktop).
GPU_PACKAGES=() NVIDIA_DRIVER=none GPU_MULTI=0

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\n\033[31mrenner-install: %s\033[0m\nLog: %s\n' "$*" "$LOG" >&2; exit 1; }
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
            --password-hash) PASSWORD_HASH=$2; shift ;;
            --no-desktop) DESKTOP=0 ;;
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
    # The ISO fills its package keyring at boot (pacman-init.service). Starting
    # pacstrap before that finishes fails the signature checks, so wait for it;
    # `systemctl start` returns once the oneshot has finished.
    if systemctl cat pacman-init.service >/dev/null 2>&1; then
        systemctl start pacman-init.service || die "the ISO's package keyring could not be set up"
    fi
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
    if [[ -z $PASSWORD && -z $PASSWORD_HASH ]]; then
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

detect_gpus() {
    say "Detecting graphics hardware"
    command -v python3 >/dev/null || pacman -Sy --noconfirm --needed python >/dev/null
    local profile rc=0
    profile=$(python3 "$STAGING/usr/bin/renner-gpu-detect") || rc=$?
    echo "$profile" | sed 's/^/    /'
    case $rc in
        0) ;;
        4) echo "    WARNING: this NVIDIA GPU needs the 580 legacy driver, which comes with the"
           echo "    renner-os package repository later. Installing the open nouveau driver for now." ;;
        *) die "GPU detection failed (exit $rc)" ;;
    esac
    eval "$(grep -E '^RENNER_GPU_(PACKAGES|NVIDIA_DRIVER|MULTI)=' <<<"$profile")"
    read -r -a GPU_PACKAGES <<<"$RENNER_GPU_PACKAGES"
    NVIDIA_DRIVER=$RENNER_GPU_NVIDIA_DRIVER
    GPU_MULTI=$RENNER_GPU_MULTI
}

install_desktop() {
    say "Installing the desktop and graphics drivers (this takes a while)"
    # multilib carries the 32-bit drivers that games and Wine/Proton need.
    sed -i '/^#\[multilib\]/,/^#Include/ s/^#//' "$TARGET/etc/pacman.conf"
    grep -q '^\[multilib\]' "$TARGET/etc/pacman.conf" || die "could not enable multilib"
    arch-chroot "$TARGET" pacman -Sy --noconfirm --needed "${DESKTOP_PACKAGES[@]}" "${GPU_PACKAGES[@]}"

    # SDDM on Wayland with KWin, like the Plasma session itself.
    install -d "$TARGET/etc/sddm.conf.d"
    cat >"$TARGET/etc/sddm.conf.d/10-renner.conf" <<'EOF'
[General]
DisplayServer=wayland
GreeterEnvironment=QT_WAYLAND_SHELL_INTEGRATION=layer-shell

[Wayland]
CompositorCommand=kwin_wayland --drm --no-lockscreen --no-global-shortcuts --locale1

[Theme]
Current=breeze
EOF
    local units=(sddm.service bluetooth.service)
    if [[ $NVIDIA_DRIVER == open ]]; then
        units+=(nvidia-suspend.service nvidia-hibernate.service nvidia-resume.service)
    fi
    ((GPU_MULTI)) && units+=(switcheroo-control.service)
    arch-chroot "$TARGET" systemctl enable "${units[@]}" >/dev/null
}

install_system_files() {
    say "Installing renner.dev OS components"
    local f mode
    for f in "${SYSTEM_FILES[@]}"; do
        mode=644
        case $f in
            usr/bin/* | usr/lib/renner-os/activate | usr/lib/renner-os/boot-health \
                | usr/lib/renner-os/initrd-etc-overlay) mode=755 ;;
        esac
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
    if [[ -n $PASSWORD_HASH ]]; then
        printf '%s:%s\n' "$USERNAME" "$PASSWORD_HASH" | arch-chroot "$TARGET" chpasswd -e
    else
        printf '%s:%s\n' "$USERNAME" "$PASSWORD" | arch-chroot "$TARGET" chpasswd
    fi
    echo '%wheel ALL=(ALL:ALL) ALL' >"$TARGET/etc/sudoers.d/10-wheel"
    chmod 440 "$TARGET/etc/sudoers.d/10-wheel"

    # renner-boot-health: a new deployment is marked good (boot counting) only
    # when its essential units run. renner-activate-* add system accounts and
    # package files in /var that a new deployment brings (see activate).
    arch-chroot "$TARGET" systemctl enable NetworkManager.service systemd-timesyncd.service \
        systemd-boot-update.service renner-activate-accounts.service renner-activate-var.service \
        renner-boot-health.service >/dev/null
}

setup_boot() {
    say "Setting up the boot loader"
    local console=""
    ((SERIAL)) && console=" console=tty0 console=ttyS0,115200"
    install -d "$TARGET/etc/kernel"
    echo "root=UUID=$ROOT_UUID rootfstype=btrfs rootflags=subvol=/@deployments/$DEPLOYMENT,compress=zstd:1,noatime ro$console" \
        >"$TARGET/etc/kernel/cmdline"

    # With the NVIDIA driver, leave out `kms` so nouveau never loads early.
    local kms=" kms"
    [[ $NVIDIA_DRIVER == open ]] && kms=""
    sed -i "s/^HOOKS=.*/HOOKS=(base systemd autodetect microcode modconf$kms keyboard sd-vconsole block filesystems renner-etc-overlay fsck)/" \
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

complete_deployment() {
    say "Finishing"
    # Same completion contract as renner-update: marker, then read-only last.
    install -Dm644 /dev/null "$TARGET/usr/lib/renner-os/deployment-complete"
    install -d "$TARGET/var/lib/renner-os/activated"
    touch "$TARGET/var/lib/renner-os/activated/$DEPLOYMENT"  # nothing to activate
    sync
    btrfs property set -ts "$TARGET" ro true
}

run_stages() {  # everything that changes the disk; leaves the target mounted
    trap 'die "failed at line $LINENO"' ERR
    check_environment
    collect_settings
    fetch_system_files
    detect_gpus
    partition_disk
    create_subvolumes
    install_packages
    if ((DESKTOP)); then install_desktop; fi
    install_system_files
    configure_system
    setup_boot
    complete_deployment
}

main() {
    parse_args "$@"
    # Open the log before anything else; without it nothing destructive runs.
    : >"$LOG" || { echo "renner-install: cannot write $LOG" >&2; exit 1; }
    # A real pipeline, so both exit codes are known and every line is in the
    # log before it is copied onto the new system.
    set +e
    run_stages 2>&1 | tee -a "$LOG"
    local status=("${PIPESTATUS[@]}")
    set -e
    if ((status[0] != 0)); then
        umount -R "$TARGET" 2>/dev/null
        exit "${status[0]}"
    fi
    ((status[1] == 0)) || echo "renner-install: warning: writing $LOG failed; the log is incomplete" >&2
    cp "$LOG" "$TARGET/var/log/renner-install.log"
    sync
    umount -R "$TARGET"
    say "renner.dev OS is installed. Remove the install medium and reboot."
}

main "$@"
