#!/bin/bash
# Prints every package of install.sh's package arrays that the repos don't have.
f=$1
pkgs=$(sed -n '/^PACKAGES=(/,/^)/p; /^DESKTOP_PACKAGES=(/,/^)/p' "$f" | grep -v '^ *#' | tr -d '()' | sed 's/^[A-Z_]*=//' | tr ' ' '\n' | grep -v '^$')
n=0; for p in $pkgs; do n=$((n+1)); pacman -Si "$p" >/dev/null 2>&1 || echo "MISSING $p"; done
echo "checked $n packages"
