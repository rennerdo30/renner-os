#!/usr/bin/env bash
# Lab: log a user in at the SDDM screen and confirm the desktop session through
# the guest agent (the password is the one place where typing is unavoidable).
#   VM=smoke lab/login.sh [USER] [PASSWORD]
set -u
cd "$(dirname "$0")/.."
VM=${VM:-smoke}
USER_NAME=${1:-tom}
PASSWORD=${2:-pw1}
L() { python lab/lab.py "$@"; }
L exec $VM --wait-agent 240 -- true >/dev/null || { echo "FAILED: guest agent"; exit 1; }
session() { L exec $VM -- sh -c "test -S /run/user/\$(id -u $USER_NAME)/wayland-0" 2>/dev/null; }
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
