#!/usr/bin/env bash
# Lab: log a user into a Plasma session and confirm it through the guest agent.
# No typing: SDDM gets a one-time autologin through the agent, is restarted,
# and the setting is removed again once the session runs (owner, 2026-11-14:
# watching the lab type wrong passwords at the greeter).
#   VM=smoke lab/login.sh [USER]
set -u
# Git Bash would rewrite /etc/... arguments into Windows paths.
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
VM=${VM:-smoke}
USER_NAME=${1:-tom}
CONF=/etc/sddm.conf.d/99-lab-autologin.conf
L() { python lab/lab.py "$@"; }
L exec $VM --wait-agent 240 -- true >/dev/null || { echo "FAILED: guest agent"; exit 1; }
# Logged in = the user's Plasma shell runs (socket names such as wayland-0/1 vary).
session() { L exec $VM -- pgrep -u "$USER_NAME" -x plasmashell >/dev/null 2>&1; }
session && { echo "already logged in"; exit 0; }
for i in $(seq 1 60); do
    L exec $VM -- systemctl is-active sddm.service >/dev/null 2>&1 && break
    sleep 2
done
# A locked account (pam_faillock, also for autologin) would refuse the login.
L exec $VM -- faillock --user "$USER_NAME" --reset >/dev/null 2>&1
L exec $VM -- sh -c "printf '[Autologin]\nUser=%s\nSession=plasma\nRelogin=false\n' '$USER_NAME' > $CONF && systemctl restart sddm.service" \
    >/dev/null || { echo "FAILED: autologin setup"; exit 1; }
ok=
for i in $(seq 1 60); do
    session && { ok=1; break; }
    sleep 2
done
L exec $VM -- rm -f "$CONF" >/dev/null
[[ -n $ok ]] || { echo "FAILED: login as $USER_NAME"; exit 1; }
echo "logged in as $USER_NAME"
