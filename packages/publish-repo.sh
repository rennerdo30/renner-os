#!/usr/bin/env bash
# Publishes a build of the renner-os repository (packages/build-repo.sh) as the
# assets of the GitHub release "repo", where installed systems update from:
#   Server = https://github.com/rennerdo30/renner-os/releases/download/repo
# Every publication needs the owner's yes (AGENTS.md).
#
#   packages/publish-repo.sh DIR      (e.g. E:/AgentData/renneros-lab/release-repo)
#
# Build DIR from a pushed, clean commit, never from the lab repository (lab
# builds carry test release numbers). New files go up first, the databases
# last, then assets no longer listed are removed: a system that syncs midway
# sees the old database with its old packages, or the new one with its new ones.
set -euo pipefail
DIR=${1:?usage: publish-repo.sh DIR}
GH_REPO=rennerdo30/renner-os
TAG=repo
cd "$DIR"
[[ -f renner-os.db && -f renner-os.db.sig ]] || { echo "$DIR has no signed renner-os.db" >&2; exit 1; }
for f in renner-os.db renner-os.files; do
    [[ ! -L $f ]] || { echo "$f is a symlink; build-repo.sh writes plain copies" >&2; exit 1; }
done
mapfile -t pkgs < <(ls -- *.pkg.tar.zst *.pkg.tar.zst.sig)
gh release view "$TAG" -R "$GH_REPO" >/dev/null
gh release upload "$TAG" -R "$GH_REPO" --clobber "${pkgs[@]}"
gh release upload "$TAG" -R "$GH_REPO" --clobber renner-os.db renner-os.db.sig renner-os.files renner-os.files.sig
keep=" ${pkgs[*]} renner-os.db renner-os.db.sig renner-os.files renner-os.files.sig "
gh release view "$TAG" -R "$GH_REPO" --json assets -q '.assets[].name' | while read -r a; do
    [[ $keep == *" $a "* ]] || { echo "removing old $a"; gh release delete-asset "$TAG" "$a" -R "$GH_REPO" -y; }
done
echo "published: $(printf '%s ' "${pkgs[@]}" | tr ' ' '\n' | grep -v '\.sig$' | tr '\n' ' ')"
