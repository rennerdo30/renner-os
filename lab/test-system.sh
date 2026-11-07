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
    out=$(L exec $VM --timeout 30 -- "$@" 2>&1 | tr '\n' ' ')
    if [[ $out =~ $want ]]; then
        printf 'ok    %-28s %s\n' "$name" "${out:0:70}"
    else
        printf 'FAIL  %-28s %s\n' "$name" "${out:0:120}"
        failed=1
    fi
}
AS=(runuser -u "$USER_NAME" --)

L exec $VM --wait-agent 180 -- true >/dev/null || { echo "FAILED: guest agent"; exit 1; }
check "sched_ext scheduler"     "lavd"           sh -c 'cat /sys/kernel/sched_ext/state /sys/kernel/sched_ext/root/ops 2>&1'
check "zram swap"               "zram0"          sh -c 'swapon --show=NAME --noheadings'
check "vm.max_map_count"        "2147483642"     sysctl -n vm.max_map_count
check "vm.swappiness"           "^180"           sysctl -n vm.swappiness
check "kernel command line"     "split_lock_detect=off" cat /proc/cmdline
check "GameMode"                "gamemoded"      sh -c 'command -v gamemoded'
check "power profiles"          "^active"         systemctl is-active power-profiles-daemon
check "systemd-oomd"            "^active"         systemctl is-active systemd-oomd
check "Steam, OBS"              "steam.*obs"     sh -c 'command -v steam; command -v obs'
check "Chrome, Zen (Flatpak)"   "(Chrome.*zen|zen.*Chrome)"    flatpak list --app --columns=application
check "Kwil (system updates)"   "no-update"      ls /usr/lib/kwil
check "no Firefox, no Kate"     "^ *$"           sh -c 'command -v firefox kate || true'
check "default browser"         "Chrome"         "${AS[@]}" xdg-mime query default x-scheme-handler/https
check "default text editor"     "kwil"           "${AS[@]}" xdg-mime query default text/plain
check "default for .exe"        "renner-exe"     "${AS[@]}" xdg-mime query default application/x-ms-dos-executable
check "Plasma style"            "renner"         sh -c 'cat /etc/xdg/plasmarc'
check "global theme"            "dev.renner"     sh -c 'grep LookAndFeelPackage /etc/xdg/kdeglobals'
check "settings pages hidden"   "kcm_kwin_effects=false" sh -c 'grep kcm_kwin_effects /etc/xdg/kdeglobals'
exit $failed
