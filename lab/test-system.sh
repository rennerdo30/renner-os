#!/usr/bin/env bash
# Lab: checks an installed, logged-in system through the guest agent: the
# performance setup (RO-5), apps and default apps, and the renner.dev look.
# Prints one line per check and exits 1 if any failed.
#   VM=smoke lab/test-system.sh
set -u
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
VM=${VM:-smoke}
USER_NAME=${USER_NAME:-tom}
L() { python lab/lab.py "$@"; }
failed=0
# check NAME EXPECTED COMMAND...   (EXPECTED: a regex the output must match)
check() {
    local name=$1 want=$2 out
    shift 2
    # Python on Windows writes \r\n; drop the \r so anchored patterns work.
    out=$(L exec $VM --timeout 30 -- "$@" 2>&1 | tr -d '\r' | tr '\n' ' ')
    if [[ $out =~ $want ]]; then
        printf 'ok    %-28s %s\n' "$name" "${out:0:70}"
    else
        printf 'FAIL  %-28s %s\n' "$name" "${out:0:120}"
        failed=1
    fi
}
# As in the user's session (environment.d sets XDG_CONFIG_DIRS there).
AS=(runuser -u "$USER_NAME" -- env XDG_CONFIG_DIRS=/etc/xdg:/usr/share/renner-os/xdg)

L exec $VM --wait-agent 180 -- true >/dev/null || { echo "FAILED: guest agent"; exit 1; }
# The lab VM has no battery: a desktop, so EEVDF and no scx_loader.
check "scheduler (desktop EEVDF)" "^disabled"    sh -c 'systemctl is-enabled scx_loader.service 2>&1; true'
check "zswap off (zram swap)"   "^N"             cat /sys/module/zswap/parameters/enabled
check "THP defrag"              "\\[defer\\+madvise\\]" cat /sys/kernel/mm/transparent_hugepage/defrag
check "vm.dirty_bytes"          "268435456"      sysctl -n vm.dirty_bytes
check "game-performance"        "game-performance" sh -c 'command -v game-performance'
check "zram swap"               "zram0"          sh -c 'swapon --show=NAME --noheadings'
check "vm.max_map_count"        "2147483642"     sysctl -n vm.max_map_count
check "vm.swappiness"           "^180"           sysctl -n vm.swappiness
check "kernel command line"     "zswap.enabled=0" cat /proc/cmdline
check "btrfs compression on /"  "compress=zstd:1" findmnt -no OPTIONS /
check "split locks not slowed"  "^0"             sysctl -n kernel.split_lock_mitigate
check "GameMode (on request)"   "gamemoderun"    sh -c 'command -v gamemoderun'
check "power profiles"          "^active"         systemctl is-active power-profiles-daemon
check "systemd-oomd"            "^active"         systemctl is-active systemd-oomd
check "Steam, OBS"              "steam.*obs"     sh -c 'command -v steam; command -v obs'
check "Chrome, Zen (Flatpak)"   "(Chrome.*zen|zen.*Chrome)"    flatpak list --app --columns=application
check "Kwil (system updates)"   "no-update"      ls /usr/lib/kwil
check "no Firefox, no Kate"     "^ *$"           sh -c 'command -v firefox kate || true'
check "default browser"         "Chrome"         "${AS[@]}" xdg-mime query default x-scheme-handler/https
check "default text editor"     "kwil"           "${AS[@]}" xdg-mime query default text/plain
check "default for .exe"        "renner-exe"     "${AS[@]}" xdg-mime query default application/x-ms-dos-executable
check "Plasma style"            "renner"         sh -c 'cat /usr/share/renner-os/xdg/plasmarc'
check "global theme"            "dev.renner"     sh -c 'grep LookAndFeelPackage /usr/share/renner-os/xdg/kdeglobals'
check "settings pages hidden"   "kcm_kwin_effects=false" sh -c 'grep kcm_kwin_effects /usr/share/renner-os/xdg/kdeglobals'
check "widget style Darkly"     "widgetStyle=Darkly" sh -c 'grep widgetStyle /usr/share/renner-os/xdg/kdeglobals'
check "window frames Darkly"    "org.kde.darkly" sh -c 'grep ^library /usr/share/renner-os/xdg/kwinrc'
check "no Breeze global themes"  "^0 *$"  sh -c 'ls -d /usr/share/plasma/look-and-feel/org.kde.breeze* 2>/dev/null | wc -l'
# The lock screen must still work without the Breeze global theme: lock, see
# the greeter run, unlock again.
check "lock screen works"       "kscreenlocker_greet" sh -c 'loginctl lock-sessions; sleep 4; pgrep -af kscreenlocker_greet; loginctl unlock-sessions'
check "renner splash"           "dev.renner"     sh -c 'cat /usr/share/renner-os/xdg/ksplashrc'
check "only renner colors"      "^RennerDark.colors *$" sh -c 'ls /usr/share/color-schemes/'
check "defaults in the image"    "renner-os/xdg"  cat /usr/lib/environment.d/40-renner-xdg.conf
check "app colors defined"      "Colors:View"    grep -o Colors:View /usr/share/renner-os/xdg/kdeglobals
exit $failed
