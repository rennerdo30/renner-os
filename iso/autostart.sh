#!/usr/bin/env bash
# renner.dev OS installer autostart, on the patched Arch Linux ISO.
#
# archiso runs this on tty1 right after the automatic root login, because the
# renner boot entry passes script=/run/archiso/bootmnt/renner/autostart.sh.
# It waits for the network, then hands over to bootstrap.sh with the
# installer and the system files from the ISO itself (no download of either).
#
# Optional kernel command line parameters (set by patch-iso.sh):
#   renner.mirror=URL   pacman mirror base, e.g. a local package cache
#   renner.source=URL   where install/ and system/ come from (default: the ISO)
#   renner.serial=1     also log to the serial console (lab)
set -uo pipefail

MEDIA=/run/archiso/bootmnt/renner

param() {  # param NAME -> value of NAME= on the kernel command line
    local p
    for p in $(</proc/cmdline); do
        [[ $p == "$1="* ]] && { echo "${p#*=}"; return 0; }
    done
    return 1
}

mirror=$(param renner.mirror) || mirror=
source=$(param renner.source) || source="file://$MEDIA/source"
serial=$(param renner.serial) || serial=0

# Typing renner-install starts it again (after connecting Wi-Fi, or after quitting).
cat >/usr/local/bin/renner-install <<EOF
#!/bin/sh
exec bash $MEDIA/autostart.sh "\$@"
EOF
chmod 755 /usr/local/bin/renner-install

if [[ -n $mirror ]]; then
    systemctl stop reflector.service 2>/dev/null
    echo "Server = $mirror/\$repo/os/\$arch" >/etc/pacman.d/mirrorlist
fi

clear
printf '\n  \033[1mrenner\033[0m\033[38;5;208m.dev\033[0m OS installer\n\n  Waiting for the network...\n'
# Online means: a default route, and the mirror's host name resolves.
host=geo.mirror.pkgbuild.com
[[ -n $mirror ]] && host=$(sed -E 's#^[a-z]+://([^/:]+).*#\1#' <<<"$mirror")
online=0
for _ in $(seq 1 60); do
    if ip route show default 2>/dev/null | grep -q . && getent ahosts "$host" >/dev/null; then
        online=1
        break
    fi
    sleep 1
done
if ((!online)); then
    cat <<'EOF'

  No network connection.

  Plug in a network cable, or connect to Wi-Fi:
      iwctl station wlan0 connect "<network name>"
  Then type:  renner-install

EOF
    exit 0
fi

export RENNER_INSTALLER_URL="file://$MEDIA/renner-installer"
export RENNER_SOURCE="$source"
[[ $serial == 1 ]] && export RENNER_SERIAL_CONSOLE=1

bash "$MEDIA/source/install/bootstrap.sh" "$@"
rc=$?
[[ $serial == 1 ]] && echo "RENNER-AUTOSTART rc=$rc" >/dev/ttyS0
if ((rc != 0)); then
    printf '\n  The installer stopped (code %s). Type renner-install to try again.\n\n' "$rc"
fi
exit "$rc"
