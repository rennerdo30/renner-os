#!/usr/bin/env bash
# Builds the signed renner-os pacman repository (RO-14) in our WSL distro:
#   renner-os     system/ of this checkout (updater, look, defaults)
#   renner-style  our Darkly fork, at the pushed head of its renner branch
#
#   wsl.exe -d renneros-build -- bash packages/build-repo.sh [renner-os] [renner-style]
#
# Output: $RENNER_REPO_OUT (default E:/AgentData/renneros-lab/artifacts/repo),
# one flat folder: packages, signatures, renner-os.db/.files (signed). Lab VMs
# get it as http://10.0.2.2:8079/artifacts/repo; it is published as a GitHub
# release only with the owner's yes.
#
# The signing key lives in $GNUPGHOME (WSL: made on the first run) and in the
# GitHub secret RENNER_REPO_SIGNING_KEY for .github/workflows/repo.yml, which
# imports it and sets RENNER_REPO_KEY_REQUIRED; it is never printed. Its public
# half is written to system/usr/share/pacman/keyrings/. RENNER_PKGREL raises
# the release number (lab: a newer build of the same commit).
set -Eeuo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${RENNER_REPO_OUT:-/mnt/e/AgentData/renneros-lab/artifacts/repo}
STYLE_GIT=${RENNER_STYLE_GIT:-https://github.com/rennerdo30/Darkly.git}
STYLE_BRANCH=${RENNER_STYLE_BRANCH:-renner}
PKGREL=${RENNER_PKGREL:-1}
export GNUPGHOME=${RENNER_REPO_GNUPGHOME:-/root/.renner-repo-gnupg}
KEY_UID="renner.dev OS packages <info@renner.dev>"
KEYRING=$REPO/system/usr/share/pacman/keyrings
WORK=/home/builder/renner-repo

say() { echo "==> $*"; }
[[ $EUID -eq 0 ]] || { echo "run as root (the WSL default user)" >&2; exit 1; }
id builder >/dev/null 2>&1 || useradd -m builder
pacman -Syu --noconfirm --needed base-devel git gnupg python >/dev/null  # never a partial upgrade

install -d -m 700 "$GNUPGHOME"
if ! gpg --batch --list-secret-keys "$KEY_UID" >/dev/null 2>&1; then
    # CI imports the key from a secret: a missing key there is an error, never a new key.
    [[ -z ${RENNER_REPO_KEY_REQUIRED:-} ]] || { echo "no signing key in $GNUPGHOME" >&2; exit 1; }
    say "Creating the repository signing key"
    gpg --batch --pinentry-mode loopback --passphrase '' \
        --quick-generate-key "$KEY_UID" ed25519 sign never 2>/dev/null
fi
FPR=$(gpg --batch --with-colons --list-secret-keys "$KEY_UID" | awk -F: '$1 == "fpr" { print $10; exit }')
# Installed systems trust one key: signing with another would break every update.
trusted=$(cut -d: -f1 "$KEYRING/renner-os-trusted" 2>/dev/null || true)
[[ -z $trusted || $trusted == "$FPR" ]] || { echo "signing key $FPR is not the trusted key $trusted" >&2; exit 1; }
mkdir -p "$KEYRING"
gpg --batch --export "$FPR" >"$KEYRING/renner-os.gpg"
echo "$FPR:4:" >"$KEYRING/renner-os-trusted"
: >"$KEYRING/renner-os-revoked"
python3 "$REPO/theme/manifest.py" >/dev/null   # the keyring files are part of system/

build() {  # build NAME VERSION [COMMIT]: fill in the PKGBUILD, makepkg as builder, sign, add
    local name=$1 ver=$2 commit=${3:-} dir=$WORK/$1 deps pkg
    if [[ -z ${RENNER_REBUILD:-} ]] && compgen -G "$OUT/$name-$ver-$PKGREL-*.pkg.tar.zst" >/dev/null; then
        say "$name $ver-$PKGREL is already in the repository"
        return 0
    fi
    rm -rf "$dir"
    install -d -o builder "$dir"
    sed -e "s/@PKGVER@/$ver/; s/@PKGREL@/$PKGREL/; s/@COMMIT@/$commit/" \
        "$REPO/packages/$name/PKGBUILD" >"$dir/PKGBUILD"
    cp "$REPO/packages/$name/"*.install "$dir/" 2>/dev/null || true
    chown -R builder "$dir"
    # Build dependencies from the Arch repositories, as root (makepkg -s needs sudo).
    deps=$(cd "$dir" && runuser -u builder -- bash -c 'source PKGBUILD; printf "%s\n" "${depends[@]}" "${makedepends[@]}"' \
        | sed 's/[<>=].*//' | sort -u)
    pacman -S --noconfirm --needed $deps >/dev/null
    (cd "$dir" && runuser -u builder -- env SYSTEM="${SYSTEM:-}" makepkg -f --noconfirm >/dev/null)
    pkg=$(ls "$dir/$name-$ver-$PKGREL-"*.pkg.tar.zst)
    gpg --batch --yes --detach-sign --no-armor -u "$FPR" "$pkg"
    mkdir -p "$OUT"
    # Older builds of this package leave the folder (the database lists one).
    find "$OUT" -maxdepth 1 -name "$name-[0-9]*.pkg.tar.zst*" -delete
    cp "$pkg" "$pkg.sig" "$OUT/"
    repo-add -q --sign --key "$FPR" "$OUT/renner-os.db.tar.gz" "$OUT/${pkg##*/}"
    say "$name $ver-$PKGREL"
}

targets=("$@")
((${#targets[@]})) || targets=(renner-os renner-style)
for t in "${targets[@]}"; do
    case $t in
        renner-os)
            # The version names the commit: 0.r<commit count>.g<hash>. Callers on
            # Windows pass it (a worktree's .git is not readable from WSL, a
            # lab snapshot has none); the package is the working tree as it is.
            version=${RENNER_OS_VERSION:-}
            if [[ -z $version ]]; then
                g() { git -c safe.directory="$REPO" -C "$REPO" "$@"; }
                version="0.r$(g rev-list --count HEAD).g$(g rev-parse --short=8 HEAD)" \
                    || { echo "no git here: set RENNER_OS_VERSION" >&2; exit 1; }
            fi
            SYSTEM=$(mktemp -d)
            cp -r "$REPO/system/." "$SYSTEM/"
            chmod -R a+rX "$SYSTEM"
            build renner-os "$version"
            rm -rf "$SYSTEM"; SYSTEM=
            ;;
        renner-style)
            commit=$(git ls-remote "$STYLE_GIT" "refs/heads/$STYLE_BRANCH" | cut -f1)
            [[ -n $commit ]] || { echo "branch $STYLE_BRANCH not found in $STYLE_GIT" >&2; exit 1; }
            tmp=$(mktemp -d)
            git clone -q --bare --filter=blob:none "$STYLE_GIT" "$tmp"
            count=$(git -C "$tmp" rev-list --count "$commit")
            rm -rf "$tmp"
            build renner-style "0.r$count.g${commit:0:8}" "$commit"
            ;;
        *) echo "unknown package $t" >&2; exit 1 ;;
    esac
done
# GitHub release assets cannot be symlinks: plain copies of the databases.
for f in renner-os.db renner-os.db.sig renner-os.files renner-os.files.sig; do
    [[ -L $OUT/$f ]] && cp --remove-destination "$(readlink -f "$OUT/$f")" "$OUT/$f"
done
ls -la "$OUT"
