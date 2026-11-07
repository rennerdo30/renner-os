# renner.dev OS: design

Status: draft, 2026-11-07. Nothing has been built or booted yet.

## Owner direction (2026-11-07)

- A daily-driver desktop distribution with **Arch Linux** as its base.
- **KDE Plasma** with a clean, prosumer theme.
- **Automatic GPU driver setup for every vendor** (AMD, Intel, NVIDIA, hybrid laptops).
- **"Native" Windows `.exe` support**: the tightest Wine/Proton integration possible at system level.
- **Immutable root filesystem**, like SteamOS.

## Architecture at a glance

```
            CI (Arch container)                       User PC
 ┌────────────────────────────────────┐    ┌──────────────────────────────────┐
 │ mkosi builds a full Arch root      │    │ ESP: systemd-boot + UKIs (A, B)   │
 │  + renner packages (packages/)     │──▶ │ root-A  (erofs, read-only)        │
 │  -> root image + UKI per variant   │    │ root-B  (erofs, read-only)        │
 │  -> signed sysupdate artifacts     │    │ data    (btrfs: /var, /home,      │
 └────────────────────────────────────┘    │          /etc overlay upper)      │
                                           └──────────────────────────────────┘
```

### Immutable base (SteamOS-style A/B)

SteamOS 3 is Arch-based too. It uses two read-only root partitions, swaps
them atomically on update and keeps `/etc`, `/var` and `/home` writable. We do
the same with upstream systemd parts instead of a custom updater:

- **Build:** `mkosi` (supports `Distribution=arch`) builds the whole root from a
  pinned Arch Linux Archive snapshot, so every image is reproducible and we
  test a snapshot before users get it. That directly fixes Arch's "an update
  broke my system" problem.
- **Layout:** ESP, `root-A`, `root-B` (erofs, read-only, optionally dm-verity),
  and one btrfs `data` partition for `/var`, `/home` and the `/etc` overlay upper
  directory (the SteamOS approach for `/etc`).
- **Updates:** `systemd-sysupdate` writes the new root into the inactive slot and
  a matching UKI into the ESP. Boot counting in `systemd-boot` (boot assessment)
  falls back to the previous slot automatically if the new one does not reach
  `boot-complete.target`.
- **Software for users:** Flatpak through Discover for apps; Distrobox (an Arch
  container) for `pacman`/AUR and CLI tools; Windows software in Wine prefixes
  under `/home`. `pacman` on the host root is read-only by design.
- **Developer escape hatch:** `renner-os unlock` mounts a writable overlay on top of
  the root until the next update (the SteamOS `steamos-readonly disable` equivalent).

Open spike (phase 1): how each UKI pins its own root slot (partition label or
verity root hash on the kernel command line), and the `/etc` 3-way merge on update.

### GPU drivers for every vendor

On an immutable system we don't install drivers at runtime. The image already
contains them, and detection picks the image **variant** and the runtime
configuration:

| Hardware | Driver | Image variant |
| --- | --- | --- |
| AMD | mesa (radeonsi, RADV), lib32 | `standard` |
| Intel | mesa (iris, ANV), intel-media-driver, lib32 | `standard` |
| NVIDIA Turing+ (RTX 20xx, GTX 16xx and newer) | `nvidia-open` + `nvidia-utils`, lib32 | `standard` |
| NVIDIA Maxwell/Pascal/Volta | `nvidia-580xx` (legacy branch, AUR; we build it in CI) | `nvidia-legacy` |
| NVIDIA Kepler and older | nouveau / NVK (mesa) | `standard` |
| Hybrid (iGPU + NVIDIA dGPU) | above + `switcheroo-control`, PRIME offload | per dGPU |

Since December 2025 Arch's main `nvidia` packages are the open kernel modules, and
the 590 driver dropped Pascal and older. `nvidia-open` and `nvidia-580xx` both ship
`nvidia.ko`, so they cannot be in one image. That is why there are two variants.

- `renner-gpu-detect` (in `packages/renner-gpu`) reads PCI display devices from
  sysfs and the NVIDIA support lists that ship with each driver branch, then
  prints the profile: variant, NVIDIA driver, hybrid flag.
- The installer runs it and installs the matching variant. The update channel
  follows the variant.
- A boot service runs it again each boot. If the hardware no longer matches the
  installed variant (for example a swapped GPU), it tells the user and offers
  `renner-os variant <name>`. It never switches silently.

### Windows `.exe` as a first-class file type

The aim: double-click (or `./app.exe`) runs it, with no prefix or runner choice in
the way. `packages/renner-exe` does this:

1. **Kernel level:** a `binfmt_misc` rule (`MZ` magic) runs PE files through
   `renner-exe`, so `./setup.exe` in a terminal just works.
2. **Desktop level:** `renner-exe.desktop` is the default handler for the exe, msi
   and lnk MIME types in KDE. Downloads don't need the exec bit for a double-click.
   Dolphin shows real exe icons through KDE's exe thumbnailer.
3. **Runner:** `umu-launcher` with a Proton build pinned **in the OS image**
   (`/usr/share/renner-os/proton/current`), so it works offline and the version
   changes only with an OS update that we tested. The DXVK, VKD3D-Proton and
   protonfixes layers come with it. Plain `wine` is the fallback.
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
- Inter for the UI, JetBrains Mono for monospace (both OFL, bundled with the
  image so there's no font fallback).
- Floating bottom panel, icon-only task manager, centred launcher, system tray kept
  minimal. Breeze as the widget base with our colour scheme and Plasma style,
  packaged as a Plasma global theme (`renner-theme`) plus SDDM and splash.
- Built as real design work (mockups first, owner sign-off), never as a stub.

## Repository layout

```
docs/DESIGN.md        this file
packages/renner-gpu/  GPU detection (installer + boot check)
packages/renner-exe/  Windows exe integration (binfmt, MIME, launcher, ntsync)
tests/                offline tests that run on any machine
lab/                  QEMU test lab on this PC (see lab/README.md)
mkosi/                image definition (phase 1)

`packages/` holds early drafts from before the plan settled: the GPU detector
has tests, and `renner-exe` has never run. Both are reviewed again in their phase.
```

## Roadmap

1. **Phase 0 (now):** design, GPU detection with tests, exe integration package.
2. **Phase 1, bootable image:** mkosi config, A/B layout, VM tests in the
   QEMU lab on this PC (`lab/`, owner 2026-11-07: QEMU on this PC, never the
   owner's WSL distros or VMs), sysupdate between two builds, rollback test.
3. **Phase 2, desktop:** Plasma with the theme, installer (Calamares or our own
   KDE installer on top of `systemd-repart`), first-boot setup.
4. **Phase 3, Windows apps:** pinned Proton in the image, the Windows apps KCM,
   start-menu integration, a test matrix of real installers.
5. **Phase 4, daily driver:** the update channel, `nvidia-legacy` variant builds,
   Secure Boot (our own key, enrolled by the installer), and the owner moving
   onto it as a daily driver.

## Owner decisions still open

- Repository hosting and visibility (public repos get GitHub-hosted CI runners for free).
- Final product name (whether "renner.dev OS" stays) and the accent colour.
