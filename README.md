# renner.dev OS

A daily-driver desktop built on **Arch Linux**: KDE Plasma with a clean
prosumer look, GPU drivers that set themselves up for every vendor, Windows
programs that start with a double-click, and a system that an update can't
break.

> **Status:** early design. There is nothing to install yet. The design is
> in [`docs/DESIGN.md`](docs/DESIGN.md).

## What it will be

- **Arch underneath.** Arch packages from Arch's own mirrors, plus a small
  signed `renner-os` repository for our own pieces, so everything updates
  through pacman.
- **Immutable like SteamOS.** A read-only root on btrfs. Updates run as a
  transaction in a new snapshot, and every earlier state stays in the boot menu
  for a one-click rollback.
- **KDE Plasma, prosumer theme.** Calm graphite tones, one accent colour,
  precise and dense, more pro tool than gaming RGB.
- **GPU drivers for every vendor.** The installer detects AMD, Intel and NVIDIA
  (including older NVIDIA cards and hybrid laptops) and installs exactly the
  right drivers.
- **Windows `.exe` as a normal file type.** Double-click or `./setup.exe` runs
  it through Proton, with prefixes, start-menu entries and logs handled for you.

## How you'll install it

1. **Install script (first).** Boot the official Arch ISO and run one command;
   the script turns the machine into renner.dev OS.
2. **Media creator (later).** A tool that adapts the official Arch ISO on your
   own machine so the stick boots straight into the installer.
3. **Our own ISO (after that).**

## Repository

| Path | What it is |
| --- | --- |
| [`docs/DESIGN.md`](docs/DESIGN.md) | Design, decisions and roadmap |
| [`lab/`](lab/README.md) | QEMU test lab for Windows: headless VMs, screenshots, keyboard input, a local pacman cache |
| `packages/renner-gpu/` | GPU detection (draft, with tests) |
| `packages/renner-exe/` | Windows exe integration (draft, never run) |
| `tests/` | Offline tests |

## Development

Tests run offline on any machine with Python 3:

```
python -m unittest discover -s tests
```

The test lab runs QEMU directly on Windows (WHPX) and keeps its data outside
the repository, in `E:/AgentData/renneros-lab`. See [`lab/README.md`](lab/README.md).

Arch Linux is a trademark of its respective owners. renner.dev OS is an
independent project, not affiliated with or endorsed by Arch Linux.
