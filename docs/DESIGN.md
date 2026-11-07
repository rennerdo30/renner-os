# renner.dev OS: design

Status: draft, 2026-11-07. Install script, immutable layout and desktop install exist and are lab-tested; audio, theme and performance are designs.

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

Added later the same day (owner, 2026-11-07, verbatim where quoted):

- **Audio that remembers:** "a proper pipe wire audio management system that
  properly saves and restores" — last time on Linux "the sessions were never
  really saved and restored". Plus "a proper volume mixer like Windows has"
  (Plasma's mixer is the base). See "Audio" below.
- **A real custom KDE theme and layout**, specced down properly, not a
  *Spielerei* (toy). See "Desktop" below.
- **An installer anyone can use, like Windows:** "an OS that anybody can install
  and use". The download installer and the later ISO installer "should have
  the same base and we develop it ourselves fully", probably a Rust app, with a
  graphical interface even on the plain Arch ISO, which has none ("if you can
  make it work that we get some cheap graphics working only for our installer
  that would be crazy good"); a TUI is acceptable as the fallback. See
  "Installer" below.
- **Unattended installs from a config file are "very important"**: specify a
  config file and the installer installs the system with no questions.
- **The desktop should feel as "Windowsy" as possible for the user**: "A
  Windows user should just feel at home. But it's not Windows ... without all
  the downsides", better in UX and in performance.
- **Performance first, stability always:** "maximum performance for drives,
  maximum performance for the graphics, maximum performance for games ... the
  best possible experience". Kernel and tuning choices are ours to make. See
  "Performance" below.

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
- **Activation** follows a manifest that `renner-update` writes into each
  deployment (`/usr/lib/renner-os/activation/`): the groups and users this
  transaction added (account lists before and after pacman, compared by name)
  and the package-owned paths under `/var` (`pacman -Qlq`). Only the manifest
  is applied, never the account databases as a whole, so deleted or renamed
  local users stay that way. It only adds, so rollback stays safe:
  - `renner-activate-accounts.service` (every boot, before `systemd-sysusers`;
    needs only `/etc`, which the initramfs mounts): checks every manifest
    entry first (an existing name must have the staged IDs; a missing one needs
    free IDs) and changes nothing on any conflict; then `groupadd`/`useradd`,
    and `pwconv`/`grpconv` for missing shadow entries.
  - `renner-activate-var.service` (first boot of each deployment; requires the
    account step to have succeeded): creates only the manifest's paths that
    do not exist in the shared `/var`, with the staged owner, mode and times.
    Existing files and directories are never touched. **Rule:** `/var` is
    state; package files there are seeded once and never updated.
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
4. **Prefixes, like one Windows (owner, 2026-11-07):** normal programs share
   **one default prefix**, so there is one C: and one `Program Files`, as on
   Windows. An exe inside an existing prefix (`…/drive_c/…`) runs in that
   prefix. Isolation only when needed (conflicting runtimes, known
   troublemakers): automatic from a compatibility list, or "Run in its own
   prefix" in Dolphin. Prefixes live in `~/.local/share/renner-os/prefixes/`.
   **Every prefix sees the same user data and drives:** `Documents`,
   `Downloads`, `Desktop`, `Pictures`, `Music` and `Videos` are the real home
   folders; `D:`, `E:` … are the same real drives with the same letters in
   every prefix; `Z:` is `/`. Steam games keep Steam's prefixes but get the
   same mapping. Later, if needed: layered prefixes (a shared runtime base
   with a thin layer per isolated program).
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
- Breeze as the widget base with our colour scheme and Plasma style, packaged
  as a Plasma global theme (`renner-theme`) plus SDDM and splash.
- Built as real design work (mockups first, owner sign-off), never as a stub.

**Familiar to Windows users (owner, 2026-11-07):** familiar *layout and
behaviour*, our own *look*.

| Windows habit | renner.dev OS default |
| --- | --- |
| Taskbar at the bottom with pinned apps; tray, clock and notifications on the right | Same layout |
| Start button and menu with search | Launcher with instant search over apps, files and settings |
| Win, Alt+Tab, Win+E, Win+L, Win+D, Win+arrows, Ctrl+Shift+Esc | Same shortcuts |
| Snap layouts | KWin tiling with the same gestures |
| Minimise, maximise, close on the right | Same |
| Explorer, Control Panel, Task Manager | Dolphin, System Settings, System Monitor, named and placed so they are found the same way |

Better than Windows by default: no ads, no telemetry, no account requirement,
no forced reboots (updates are transactional with rollback), fast search, and
one consistent look across all settings.

Not copied, for legal reasons and for our own identity: Microsoft logos, icons,
fonts (Segoe), sounds or exact visuals (no third-party trademarks in shipped
files). The visual identity stays the prosumer direction above.

### Installer: one Rust app for every path (design, not built yet)

One program, `renner-installer`, for the download path, the media creator and
our own ISO. Only the way it draws differs.

| Part | What it is |
| --- | --- |
| `installer-core` | The install engine with no UI: disk detection and the partition plan, btrfs layout, packages, GPU drivers, boot loader, users, progress events and a resumable log. It takes over every stage of `install/install.sh`. |
| GUI (Slint) | On the plain Arch ISO: Slint's LinuxKMS backend draws straight to the screen through DRM/KMS and reads input through libinput, with no X11 or Wayland, using the software renderer, so no GPU driver is needed (DRM dumb buffers, falling back to `/dev/fb*`). On our own ISO: the same UI in a normal window. |
| TUI (ratatui) | Fallback when no usable display is found, and for serial consoles. |
| Unattended | A first-class mode, not a test hook: a documented TOML config (disk, partitioning choice, users, locale, timezone, keymap, extra packages, options) and no questions. Passwords only as hashes. `--check` validates the config and prints the exact plan without touching anything. The lab tests run this path, so it is the most tested one. |

- **Flow, like Windows:** language and keyboard → network (Wi-Fi if needed) →
  disk (erase the whole disk; "next to Windows" later) → user and password →
  summary → install with real progress → reboot. Sensible defaults, nothing to
  type into a terminal. Mockups first, owner sign-off, in the theme's design
  language.
- **Download path:** the one-liner fetches a small bootstrap that verifies the
  signed installer binary before running it.
- **Risk:** Slint calls the LinuxKMS backend experimental, so the TUI
  fallback ships from the start and both are tested in the lab (which has a
  virtio GPU and a serial console).
- `install/install.sh` stays the reference and lab tool until the Rust
  installer passes the same lab tests for every stage.

### Audio: PipeWire that saves and restores (design, not built yet)

The owner's experience: settings never reliably survived sessions. The usual
causes are not missing persistence but unstable device identity (a USB headset
in another port, HDMI after a monitor switch, Bluetooth reconnects get new node
names), restore features that are off or partial, and no rule for which device
becomes default when several come and go. Plan, in order:

1. **WirePlumber's own memory fully on and tested:** default devices, device
   profiles and routes (ports), per-app stream volume, mute and target device,
   all stored in `~/.local/state/wireplumber` (on `@home`, so updates and
   rollbacks never touch it).
2. **Stable device identity:** WirePlumber rules that name and match devices by
   stable properties (serial number, bus path, Bluetooth address) instead of
   enumeration order, so a remembered setting finds its device again.
3. **Device priority:** the user ranks outputs and inputs (for example headset
   > speakers > HDMI). When a higher-ranked device appears, it becomes the
   default again; when it goes, the next one takes over. Small service on top
   of WirePlumber, configured from the mixer UI.
4. **Mixer:** Plasma's mixer (per-app volume, device switch, profiles) styled to
   our theme; anything missing compared to the Windows mixer is added there,
   not as a separate app.
5. **Tests that can fail:** lab scenarios with virtual devices: set volumes and
   defaults, reboot, unplug/replug, update, roll back; every value must come
   back. Then real hardware with USB, HDMI and Bluetooth.

Before building: check upstream WirePlumber issues and existing tools for each
failure mode, and write down which of them the stock configuration already
handles (history first, price before building).

### Performance (design, not built yet)

Stability first, then the most performance that is measurable. Plan:

| Area | Choice | Reason |
| --- | --- | --- |
| Kernel | `linux-zen` by default, stock `linux` as a second boot entry | Official Arch package with desktop-oriented tuning; nothing self-compiled. CachyOS-style builds (`-O3`, x86-64-v3) only with our own repo and only if measured worth it. |
| CPU scheduler | `sched_ext` with `scx_lavd` (`scx-scheds`) | Built for interactive and gaming loads (Steam Deck); switchable at runtime. |
| Storage | NVMe `none`, SATA SSD `mq-deadline`, HDD `bfq` via udev; btrfs `noatime`, `zstd:1` | The right scheduler per device type. |
| Memory | zram swap (`zram-generator`) | Compressed RAM swap instead of disk swap: no stalls under memory pressure. |
| Games | `ntsync`, `gamemode`, `kernel.split_lock_mitigate=0` | Faster Wine synchronisation; split-lock mitigation slows some Windows games. |
| Mitigations | Kept on | A few percent is not worth real security holes on a daily driver. |

Every tweak needs a before/after measurement on real hardware (game frame
times and FPS, boot time, disk throughput, input latency) and stays only if it
helps. The lab VM cannot measure performance. With nvidia-open, a non-default
kernel needs `nvidia-open-dkms` and its headers.

### Theme and layout: the plan (design, not built yet)

Specced down before building, so it is a product and not a toy:

1. **Design spec:** tokens (colour scales for light and dark, the one accent,
   type scale with Inter and JetBrains Mono, spacing, radii, borders,
   elevation, motion), with written rules for where each is used.
2. **Mockups of every surface:** desktop and panel, launcher, task switcher,
   notifications, volume and network popups, system settings, Dolphin, Konsole,
   window decorations, SDDM login, lock screen, splash. Owner sign-off first.
3. **Implementation as packages:** a Plasma global theme (colour scheme, Plasma
   style, window decoration, cursor, icons, wallpapers, splash, SDDM theme),
   the panel layout as a look-and-feel layout script, and KWin, font and
   behaviour defaults.
4. **Checks:** screenshots of every surface in light and dark in the lab,
   compared against the mockups.

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
