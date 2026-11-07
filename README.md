# renner.dev OS

A daily-driver desktop built on **Arch Linux**: KDE Plasma reworked into a calm,
Windows-familiar desktop, GPU drivers that set themselves up, Windows programs
that start with a double-click, and a read-only system that an update can't
break.

> **Status: working prototype, not released.** It installs and runs in the
> test lab (QEMU) from a patched Arch ISO. There is no public download yet,
> real hardware is not tested, and the license is not decided (RO-15).

## What works today

- **Installer** (Rust, Slint) on the Arch ISO, usable with mouse, keyboard or
  touch alone (on-screen keyboard):
  - language, keyboard layout and time zone;
  - network, with a Wi-Fi list (iwd);
  - disk, account, summary, install with progress;
  - unattended installs from a config file ([`docs/unattended.md`](docs/unattended.md)).
- **Immutable system.** The root is a read-only btrfs deployment.
  `renner-update` installs updates into a new deployment, and the boot menu
  keeps the earlier ones. If a new one fails to boot three times, the system
  falls back to the previous one by itself. `/etc` is a writable overlay.
- **Look and desktop (renner.dev):**
  - our own color scheme, Plasma style, global theme, wallpaper and login splash;
  - the Darkly widget style; Inter and Papirus;
  - a Windows-like taskbar and tray, and a slim "show desktop" edge;
  - **renner.dev Start:** search first, with pinned apps and all apps A–Z;
    the Windows key opens it;
  - the stock KDE themes are removed, and System Settings is trimmed to what
    a Windows user looks for.
- **Windows programs.** A double-click on an `.exe` (or `./setup.exe`) runs it
  through **GE-Proton**, which ships with the system:
  - no Steam and nothing to download on first start;
  - one shared C: for all programs;
  - the home folders appear as Documents, Downloads and so on;
  - installed programs show up in the start menu under "Windows apps".
- **Apps out of the box:**
  - Steam, OBS, Chrome (default browser) and Zen;
  - Kwil (renner.dev's editor, the default for text and code);
  - the renner.dev Launcher;
  - Discover with Flathub.
- **Performance** (defaults from Bazzite and CachyOS, see RO-5):
  - zram swap, with zswap off;
  - `scx_lavd` on laptops, EEVDF on desktops;
  - I/O scheduler per drive;
  - memory and THP tuning, systemd-oomd, power profiles;
  - `game-performance` for Steam (`game-performance %command%`).
- **GPU drivers.** AMD, Intel and NVIDIA are detected by PCI ID, and the right
  driver is installed, including runtime power management on NVIDIA laptops.

## Installing (lab and testing)

- **Patched Arch ISO (recommended).** [`iso/patch-iso.sh`](iso/patch-iso.sh)
  takes the official Arch ISO and adds the installer as the default boot
  entry. It leaves Arch's own files untouched, and the ISO boots straight into
  the installer.
- **On the plain Arch ISO.** [`install/bootstrap.sh`](install/bootstrap.sh)
  downloads the installer, checks it and starts it (`curl … | bash`). This needs
  a published installer build; until the renner-os repository exists (RO-14),
  that only works in the lab.

## Repository

| Path | What it is |
| --- | --- |
| [`docs/DESIGN.md`](docs/DESIGN.md) | Design, decisions and roadmap |
| [`installer/`](installer/) | Rust installer: `installer-core` (config, detection, plan, engine) and `renner-installer` (GUI and CLI) |
| [`install/`](install/) | `install.sh` (what the installer runs) and `bootstrap.sh` |
| [`iso/`](iso/) | ISO patcher and the ISO's autostart |
| [`system/`](system/) | Files the installer puts on the new system; [`system/MANIFEST`](system/MANIFEST) lists them |
| [`theme/`](theme/) | Generators for colors, Plasma style, wallpaper and panel layout |
| [`packages/`](packages/) | Builds of packages we ship ourselves (Darkly) |
| [`lab/`](lab/) | Test lab: QEMU on Windows, WSL builds, automated install and system tests |
| `tests/` | Offline unit tests |

## Development

Builds run in a dedicated Arch WSL distro (`renneros-build`); test machines
run in QEMU (WHPX) on Windows. Lab data lives outside the repository, in
`E:/AgentData/renneros-lab`.

```
lab/cycle.sh [exe]        build in WSL, install a VM from the patched ISO, optionally run a Windows program
lab/test-system.sh        check the installed system through the QEMU guest agent
lab/test-look.sh          screenshots of the start menu, Dolphin and Settings
lab/theme-push.sh         fast theme iteration on an installed VM (no reinstall)
```

Lab automation rules (control channel and guest agent, every step checked)
are in [`AGENTS.md`](AGENTS.md).

Offline checks run on any machine:

```
python -m unittest discover -s tests
cargo test --manifest-path installer/Cargo.toml -p installer-core
python theme/manifest.py --check
```

Arch Linux is a trademark of its respective owners. renner.dev OS is an
independent project, not affiliated with or endorsed by Arch Linux.
