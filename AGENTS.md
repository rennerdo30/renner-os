# renner.dev OS: agent rules

Read `E:/Development/AGENTS.md` and `E:/Development/general-agent-rules.md` first; this file adds the rules of this repository.

## Lab automation (owner, 2026-11-08: "make it a rule")

Tests drive the system through control channels that answer, never through blind input:

- **Installer:** the lab control channel (`renner.test=1`, second serial port; `lab.py ui VM state|set|pick|next|back`, `lab.py ui VM --wait-page N`). Never click screen coordinates or type keys to move through the installer.
- **Installed system:** the QEMU guest agent (`lab.py exec VM -- CMD`, exit code and output). Programs in the user's desktop session start with `runuser -u USER -- env XDG_RUNTIME_DIR=… DBUS_SESSION_BUS_ADDRESS=… systemd-run --user …`, not by typing into KRunner or a terminal.
- **Every step checks its result** (a page reached, an exit code, a file, a process) and the test stops with a screenshot and the reason at the first failure. No step relies on a fixed `sleep` to have worked.
- **Wait for states, not for time:** poll the state with a timeout (`--wait-page`, `--wait-agent`, a loop over a check), never a bare sleep followed by the next action.
- **Login without typing:** `lab/login.sh` sets a one-time SDDM autologin through the guest agent, restarts SDDM and removes the setting once Plasma runs. Never type a password into the greeter.
- **Typing and clicking stay only where nothing else exists** (a deliberate mouse-only or keyboard-only UI test). Each such action is followed by a check of its effect through a channel, with a bounded retry.
- **One cycle, one snapshot:** `lab/cycle.sh` builds and tests `git archive HEAD`, so edits during a run cannot mix into it. Shut a VM down before rebuilding the ISO it uses.
- **Builds run in the `renneros-build` WSL distro** (`lab/wsl-build.sh`), test VMs run in QEMU with `--window` (owner, 2026-11-07).
