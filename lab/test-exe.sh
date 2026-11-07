#!/usr/bin/env bash
# Lab: run a Windows program in an installed, logged-in VM the way a
# double-click does (renner-exe in the user's desktop session), through the
# QEMU guest agent: no typing into the desktop, every step checks its result.
#   VM=smoke lab/test-exe.sh [URL]      default: the Battle.net installer
set -u
cd "$(dirname "$0")/.."
VM=${VM:-smoke}
USER_NAME=${USER_NAME:-tom}
URL=${1:-https://downloader.battle.net/download/getInstaller?os=win&installer=Battle.net-Setup.exe}
L() { python lab/lab.py "$@"; }
S=E:/AgentData/renneros-lab/vms/$VM/shots
fail() { echo "FAILED: $*"; L shot $VM $S/E-FAILED.png >/dev/null; exit 1; }
X() { L exec $VM --timeout "${T:-60}" -- "$@"; }

L exec $VM --wait-agent 180 -- true || fail "guest agent"
uid=$(X id -u "$USER_NAME") || fail "no user $USER_NAME"
# The desktop session must be up (Plasma imports its environment into the
# user's systemd, which is where the program is started).
for i in $(seq 1 60); do
    X test -S "/run/user/$uid/wayland-0" && break
    sleep 2
done
X test -S "/run/user/$uid/wayland-0" || fail "no desktop session for $USER_NAME"
AS=(runuser -u "$USER_NAME" -- env "XDG_RUNTIME_DIR=/run/user/$uid" "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$uid/bus")

echo "== download"
T=180 X "${AS[@]}" curl -fsSL -o "/home/$USER_NAME/Downloads/test.exe" "$URL" || fail "download"
echo "== start (as a double-click would)"
X "${AS[@]}" systemd-run --user --collect --unit=renner-exe-test /usr/bin/renner-exe "/home/$USER_NAME/Downloads/test.exe" \
    || fail "could not start renner-exe"
start=$SECONDS
for i in $(seq 1 24); do
    sleep 5
    L shot $VM $S/E-$i.png >/dev/null
    state=$(X "${AS[@]}" systemctl --user is-active renner-exe-test.service 2>/dev/null)
    [[ $state == active ]] || break
    # Running for a minute with the program's process alive counts as started.
    if ((SECONDS - start > 60)) && X pgrep -u "$USER_NAME" -f test.exe >/dev/null; then
        echo "running after $((SECONDS - start)) s"
        break
    fi
done
L shot $VM $S/E-last.png >/dev/null
echo "== renner-exe log"
X sh -c "tail -n 25 /home/$USER_NAME/.local/state/renner-os/exe/test.log"
X pgrep -u "$USER_NAME" -f test.exe >/dev/null || fail "the program is not running"
echo test-exe-done
