#!/usr/bin/env bash
# Lab: log a user in at the SDDM screen and confirm the desktop session through
# the guest agent (the password is the one place where typing is unavoidable).
#   VM=smoke lab/login.sh [USER] [PASSWORD]
set -u
# Git Bash would rewrite /home/... arguments into Windows paths.
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
VM=${VM:-smoke}
USER_NAME=${1:-tom}
PASSWORD=${2:-pw1}
L() { python lab/lab.py "$@"; }
L exec $VM --wait-agent 240 -- true >/dev/null || { echo "FAILED: guest agent"; exit 1; }
# Logged in = the user's Plasma shell runs (socket names such as wayland-0/1 vary).
session() { L exec $VM -- pgrep -u "$USER_NAME" -x plasmashell >/dev/null 2>&1; }
session && { echo "already logged in"; exit 0; }
# SDDM must be on screen before typing.
for i in $(seq 1 60); do
    L exec $VM -- systemctl is-active sddm.service >/dev/null 2>&1 && break
    sleep 2
done
sleep 5
for attempt in 1 2 3; do
    L type $VM "$PASSWORD" --enter
    for i in $(seq 1 30); do
        session && { echo "logged in as $USER_NAME (attempt $attempt)"; exit 0; }
        sleep 2
    done
done
echo "FAILED: login as $USER_NAME"
exit 1
