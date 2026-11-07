# renner.dev OS: design

Status: draft, 2026-11-07. Only the test lab and two drafts exist; no OS code yet.

## Owner direction (2026-11-07)

- A daily-driver desktop distribution with **Arch Linux** as its base.
- **KDE Plasma** with a clean, prosumer theme.
- **Automatic GPU driver setup for every vendor** (AMD, Intel, NVIDIA, hybrid laptops).
- **"Native" Windows `.exe` support**: the tightest Wine/Proton integration possible at system level.
- **Immutable root filesystem**, like SteamOS (as far as an install script allows; see below).
- **Delivery, in three steps:**
  1. **Install script only.** The user boots the official Arch ISO and runs our
     web install script, which sets Arch up the way we want it. We do not
     distribute our own ISO.
  2. **Media creator.** A tool that takes the official Arch ISO and adapts it
     on the user's own machine, so the stick boots straight into our installer.
  3. **Our own ISO.**

## Architecture at a glance

```
 Official Arch ISO (booted by the user)
   └─ curl -fsSL <renner.dev URL> | sh        (step 1)
        ├─ partition + btrfs subvolumes
        ├─ pacstrap: Arch packages from Arch mirrors
        ├─ renner-os repo: our packages (theme, exe integration, GPU, updater)
        ├─ GPU detection -> exactly the right driver packages
        └─ bootloader with snapshot entries, read-only root
```

Our own pieces ship as **packages in a small pacman repository (`renner-os`)**,
signed with our key. The installer only adds that repo and installs a
meta-package. That way every part of renner.dev OS updates through pacman like
the rest of Arch, and the install script stays small.

### Install script (step 1)

- Runs on the official Arch ISO as root and asks only a few questions: disk,
  user, password, timezone and locale with sensible defaults, then a summary
  before anything is written.
- Idempotent stages with a log (`/var/log/renner-install.log` on the target),
  so a failed run can be read and resumed.
- The script is downloaded over HTTPS and its signature is checked before it runs.
  The one-liner fetches a small bootstrap that verifies the real installer.
- Publishing the script and the repo needs the owner's yes (rules: releases and
  publication).

### Immutability with a script install

True SteamOS-style A/B depends on shipping finished images, and step 1 does not.
The closest match with plain Arch packages:

- **Read-only root on btrfs, updates as a transaction** (owner decision
  2026-11-07). `renner-update` snapshots the root, runs `pacman -Syu`
  inside the new snapshot, and makes it the default for the next boot. The
  running system never changes mid-update. The update is all or nothing.
- **Every earlier state is a boot menu entry**, so rolling back is one pick at
  boot. This is the openSUSE Aeon / `transactional-update` model.
- `/home` and `/var` stay writable subvolumes outside the deployments.

#### Implemented layout (install/install.sh, system/)

| Piece | Where | Notes |
| --- | --- | --- |
| ESP (2 GiB, FAT32) | `/efi` | systemd-boot, one UKI per deployment in `EFI/Linux/` |
| `@deployments/<id>` | `/` (read-only) | chosen by `rootflags=subvol=` in each UKI |
| `@home` | `/home` | |
| `@var` | `/var` | shared; `/root` and `/usr/local` are bind mounts from it |
| `@etc-overlay` | upper layer of `/etc` | mounted in the initramfs (`renner-etc-overlay`) |
| pacman database | `/usr/lib/sysimage/pacman` | inside the deployment, so a rollback restores it; `/var/lib/pacman` is a symlink |

- **`/etc`:** an overlay with the deployment's `/etc` as the lower layer and
  `@etc-overlay` as the upper. Files the user changed stay changed across
  updates; every other file comes from the booted deployment. It is mounted
  with `index=off`, because the lower layer changes with every update
  (overlayfs otherwise refuses: "failed to verify upper root origin").
  The overlay is required: if it cannot be mounted, boot stops in the
  emergency shell rather than running with the deployment's default `/etc`.
  `renner.etc=off` on the kernel command line is the deliberate repair bypass
  (read-only `/etc`).
- **`renner-update`** never changes the running system:
  1. snapshot of the booted deployment, with a monotonic ID (UTC, always above
     the newest existing ID);
  2. the effective configuration is mirrored from the live `/etc` into it,
     deletions included: pacman config, the boot configuration (kernel command
     line, mkinitcpio, modprobe, modules-load, vconsole) and the account
     databases;
  3. `pacman` in a chroot of the snapshot with the snapshot's **own** `/var`
     (only the package cache and the logs are shared), then a new UKI;
  4. the UKI goes to the ESP with 3 tries (`renner-<id>+3.efi`), a completion
     marker is written, and setting the snapshot read-only is the last step.
  A failed transaction deletes its snapshot and boot entry; anything left
  unfinished is removed by the next update. It keeps 3 finished deployments
  plus the booted one.
- **Activation** only adds, so rollback stays safe:
  - `renner-activate-accounts.service` (every boot, before `systemd-sysusers`;
    needs only `/etc`, which the initramfs mounts): system groups and users that
    the deployment's own `/etc` has but the live `/etc` lacks are added with the
    exact IDs from staging; `pwconv`/`grpconv` fill in missing shadow entries
    when only some account files are in the overlay's upper layer. An ID taken
    by another name fails the unit.
  - `renner-activate-var.service` (first boot of each deployment, after
    `local-fs.target`, before `systemd-tmpfiles-setup`): files that packages
    ship under `/var` are copied into the shared `/var` without overwriting.
  - Ordering matters: `systemd-sysusers` runs before `local-fs.target`, so one
    unit ordered after `local-fs.target` and before `sysusers` is a cycle (a lab
    boot hung on it); hence two units.
- **Boot health and rollback:** pick an older entry in the boot menu at any
  time. A new deployment is marked good only when `renner-boot-health` finds
  the essential units active (`/usr/lib/renner-os/essential.units`: activation,
  journald, dbus, NetworkManager, SDDM when enabled); an unrelated optional unit
  failing does not count. Without that mark after 3 tries, systemd-boot falls
  back to the previous deployment by itself.
- **Software for users:** Flatpak through Discover for apps; Distrobox (an Arch
  container) for AUR and CLI tools; Windows software in Wine prefixes in
  `/home`. Host `pacman` runs only through `renner-update`.
- **Escape hatch:** `renner-os unlock` makes the current root writable until the
  next update, for developers.
- Rejected alternative: a writable root plus automatic snapshots before every
  update (like CachyOS). It is easier but not immutable.

Steps 2 and 3 keep this layout, so an install from the media creator or our own
ISO behaves the same.

### GPU drivers for every vendor

The installer detects the GPUs and installs exactly the right drivers. A boot
check warns when the hardware changes later (for example a swapped GPU) and
offers the fix; it never switches drivers silently.

| Hardware | Driver packages |
| --- | --- |
| AMD | mesa (radeonsi, RADV), lib32 |
| Intel | mesa (iris, ANV), intel-media-driver, lib32 |
| NVIDIA Turing+ (RTX 20xx, GTX 16xx and newer) | `nvidia-open` + `nvidia-utils`, lib32 |
| NVIDIA Maxwell/Pascal/Volta | `nvidia-580xx` (legacy branch, AUR; prebuilt in our repo) |
| NVIDIA Kepler and older | nouveau / NVK (mesa) |
| Hybrid (iGPU + NVIDIA dGPU) | above + `switcheroo-control`, PRIME offload |

Since December 2025 Arch's main `nvidia` packages are the open kernel modules, and
the 590 driver dropped Pascal and older. `nvidia-open` and `nvidia-580xx` both ship
`nvidia.ko`, so a machine gets one or the other. One legacy card moves the whole
machine to the 580 branch, because it also drives newer GPUs.

`renner-gpu-detect` (draft in `packages/renner-gpu`, with tests) reads PCI display
devices from sysfs and NVIDIA's own support lists, and prints the profile the
installer acts on.

### Windows `.exe` as a first-class file type

The aim: double-click (or `./app.exe`) runs it, with no prefix or runner choice in
the way. `packages/renner-exe` (draft, never run) does this:

1. **Kernel level:** a `binfmt_misc` rule (`MZ` magic) runs PE files through
   `renner-exe`, so `./setup.exe` in a terminal just works.
2. **Desktop level:** `renner-exe.desktop` is the default handler for the exe, msi
   and lnk MIME types in KDE. Downloads don't need the exec bit for a double-click.
   Dolphin shows real exe icons through KDE's exe thumbnailer.
3. **Runner:** `umu-launcher` with a Proton build that we pin and ship as a
   package in our repo, so it works offline and changes only with an update we
   tested. The DXVK, VKD3D-Proton and protonfixes layers come with it. Plain
   `wine` is the fallback.
4. **Prefixes:** an exe inside an existing prefix (`…/drive_c/…`) runs in that
   prefix. Anything else uses the shared default prefix. "Run in its own prefix"
   is a Dolphin context-menu action. Prefixes live in
   `~/.local/share/renner-os/prefixes/`.
5. **Integration:** installed Windows programs get start-menu entries (Wine
   already writes `.desktop` files; we re-home them under a "Windows" category).
   Logs go to `~/.local/state/renner-os/exe/`, and a failure shows a KDE
   notification with an "Open log" action.
6. **Kernel and packages:** the `ntsync` module loads at boot (Linux 6.14+, used by
   current Wine and Proton), and multilib (32-bit libraries) is on by default. Arch
   already raises `vm.max_map_count` for games, so we don't change it.

Later: a KDE settings module ("Windows apps") to list prefixes, pick the Proton
version, enable per-app tweaks and clean prefixes.

### Desktop: KDE Plasma with a prosumer theme

Direction: calm, dense, precise, like a pro tool and not a gaming RGB look.

- Graphite neutrals (light and dark), **one** accent colour, no gradients, subtle
  1 px separators, 6 px radii, a tight but readable density.
- Inter for the UI, JetBrains Mono for monospace (both OFL, installed as
  dependencies so there's no font fallback).
- Floating bottom panel, icon-only task manager, centred launcher, system tray kept
  minimal. Breeze as the widget base with our colour scheme and Plasma style,
  packaged as a Plasma global theme (`renner-theme`) plus SDDM and splash.
- Built as real design work (mockups first, owner sign-off), never as a stub.

### Media creator (step 2)

Takes the official Arch ISO (downloaded and signature-checked by the tool) and
adapts it on the user's machine so the stick starts our installer. We never
distribute a modified Arch ISO. To be designed when step 1 works.

## Test lab

QEMU directly on this Windows PC (owner 2026-11-07; never the owner's WSL
distros or VMs). See `lab/README.md`. It includes a local pacman package cache
so repeated test installs don't download every package again.

## Repository layout

```
docs/DESIGN.md        this file
install/              the install script (step 1, next)
packages/renner-gpu/  GPU detection (draft with tests)
packages/renner-exe/  Windows exe integration (draft, never run)
tests/                offline tests that run on any machine
lab/                  QEMU test lab and package cache
```

## Roadmap

1. **Step 1, install script:** script on the official ISO, btrfs layout with
   read-only root and `renner-update`, GPU drivers, Plasma, the `renner-os` repo
   with our packages, Windows exe integration. Tested end to end in the lab.
2. **Step 2, media creator** for the official Arch ISO.
3. **Step 3, our own ISO.**

Across all steps: the theme as real design work, Secure Boot (our own key,
enrolled by the installer), and the owner moving onto it as a daily driver.

## Owner decisions

- 2026-11-07: immutability via a read-only btrfs root with transactional updates.
- 2026-11-07: the project, the install script and the `renner-os` package repo
  live in a **public GitHub repository**. CI runs on GitHub-hosted runners (free
  for public repos; no self-hosted runner for a public repo). Each publication
  (creating the repo, a script release, the package repo going live) still
  needs the owner's yes.

Still open: final product name (whether "renner.dev OS" stays) and the accent colour.
