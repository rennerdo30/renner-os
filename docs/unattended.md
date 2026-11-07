# Unattended installs

`renner-installer --config install.toml` installs renner.dev OS with no
questions. `--check` validates the file against the machine and prints the
exact plan without changing anything; use it first.

```
renner-installer --config install.toml --check   # plan only
renner-installer --config install.toml           # install
renner-installer --config install.toml --verbose # install, print every line
```

## The config file

```toml
[disk]
# The whole disk to install on. Aliases such as /dev/disk/by-id/... are
# resolved. Partitions, mounted disks and the install medium are refused.
device = "/dev/nvme0n1"
# Must be true: the whole disk is erased.
erase = true

[system]
hostname = "renner"            # 1-63 letters, digits or hyphens
timezone = "Europe/Berlin"     # default UTC
locale = "en_US.UTF-8"         # default en_US.UTF-8
keymap = "de"                  # console keymap, default us

[user]
name = "alex"                  # lowercase; becomes an administrator
# A crypt hash, never a plain password:
#   openssl passwd -6
password_hash = "$6$...."

[options]                      # all optional
desktop = true                 # KDE Plasma and the detected GPU drivers (default)
serial_console = false         # kernel console on ttyS0 too (VMs)
source = "https://raw.githubusercontent.com/rennerdo30/renner-os/main"
```

## Rules

- Unknown keys are errors, so a typo never silently falls back to a default.
- All problems are reported at once.
- The machine checks come from the same engine the graphical installer uses:
  UEFI boot, a whole disk of at least 32 GiB that is not mounted and is not the
  install medium.
- The installer stays the same program for the download path, the media
  creator and our own ISO; only the frontend differs.
