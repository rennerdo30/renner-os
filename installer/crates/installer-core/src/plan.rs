//! The install plan: config + machine → exactly what will happen. `--check`
//! prints it; the GUI shows it on the summary page. Building a plan never
//! changes anything.

use crate::config::InstallConfig;
use crate::detect::{Disk, Inventory};
use anyhow::{Result, bail};
use std::fmt;

/// Smallest disk we install on: a desktop, three deployments and room to grow.
pub const MIN_DISK_BYTES: u64 = 32 * (1 << 30);

#[derive(Debug, Clone)]
pub struct Plan {
    pub config: InstallConfig,
    pub disk: Disk,
    pub gpu_summary: String,
    pub packages_extra: Vec<String>,
}

/// `resolved_device` is `config.disk.device` with aliases resolved (the caller
/// canonicalizes it on the target machine, so this stays testable).
pub fn build(config: &InstallConfig, inventory: &Inventory, resolved_device: &str) -> Result<Plan> {
    config.validate()?;
    let mut problems = Vec::new();
    if !inventory.uefi {
        problems.push("this machine booted in BIOS mode; renner.dev OS needs UEFI".to_string());
    }
    let disk = inventory.disks.iter().find(|d| d.path == resolved_device).cloned();
    match &disk {
        None => problems.push(format!(
            "{} ({resolved_device}) is not a whole disk on this machine; disks: {}",
            config.disk.device,
            inventory.disks.iter().map(|d| d.path.as_str()).collect::<Vec<_>>().join(", ")
        )),
        Some(d) if d.install_medium => problems.push(format!("{} is the install medium", d.path)),
        Some(d) if d.in_use => problems.push(format!("{} has mounted partitions", d.path)),
        Some(d) if d.size_bytes < MIN_DISK_BYTES => {
            problems.push(format!("{} has {:.0} GiB; at least 32 GiB are needed", d.path, d.size_gib()))
        }
        Some(_) => {}
    }
    if !problems.is_empty() {
        bail!("the install cannot start:\n  - {}", problems.join("\n  - "));
    }
    let g = &inventory.gpu;
    let gpu_summary = gpu_summary(g);
    Ok(Plan {
        config: config.clone(),
        disk: disk.expect("checked above"),
        gpu_summary,
        packages_extra: if config.options.desktop { g.packages.iter().map(|s| s.to_string()).collect() } else { vec![] },
    })
}

/// The graphics hardware and driver in plain words, e.g.
/// "NVIDIA graphics · NVIDIA driver (open modules)" or
/// "Intel graphics + NVIDIA graphics · NVIDIA driver (open modules), switchable".
pub fn gpu_summary(g: &crate::detect::GpuProfile) -> String {
    if g.gpus.is_empty() {
        return "No graphics card found · standard driver".to_string();
    }
    let mut names: Vec<&str> = g
        .gpus
        .iter()
        .map(|x| match x.vendor {
            "amd" => "AMD graphics",
            "intel" => "Intel graphics",
            "nvidia" => "NVIDIA graphics",
            _ => "Virtual graphics",
        })
        .collect();
    names.dedup();
    let driver = match g.nvidia_driver {
        "open" => "NVIDIA driver (open modules)",
        "legacy-580" => "open nouveau driver for now (this NVIDIA card needs the 580 driver)",
        "nouveau" => "open nouveau driver",
        _ if g.gpus.iter().all(|x| x.vendor == "other") => "standard driver",
        _ => "open Mesa drivers",
    };
    let switchable = if g.gpus.len() > 1 { ", switchable" } else { "" };
    format!("{} · {driver}{switchable}", names.join(" + "))
}

impl Plan {
    /// The plan as label/value rows for the graphical summary page.
    pub fn rows(&self) -> Vec<(String, String)> {
        let c = &self.config;
        let mut rows = vec![
            ("Disk".to_string(), format!("{} ({})", self.disk.title(), self.disk.path)),
            ("Computer name".to_string(), c.system.hostname.clone()),
            ("Account".to_string(), format!("{} (administrator)", c.user.name)),
            ("Region".to_string(), format!("{} · {} · keyboard {}", c.system.timezone, c.system.locale, c.system.keymap)),
        ];
        if c.options.desktop {
            rows.push(("Desktop".to_string(), "KDE Plasma".to_string()));
            rows.push(("Graphics".to_string(), self.gpu_summary.clone()));
        } else {
            rows.push(("Desktop".to_string(), "None (console system)".to_string()));
        }
        rows.push(("System".to_string(), "Read-only, with safe updates and rollback".to_string()));
        rows
    }

    /// The one-line erase warning for the summary page.
    pub fn erase_warning(&self) -> String {
        format!("Everything on {} ({}) will be erased.", self.disk.title(), self.disk.path)
    }
}

impl fmt::Display for Plan {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let c = &self.config;
        writeln!(f, "renner.dev OS install plan")?;
        writeln!(f, "  Disk:      {}  ALL DATA ON IT WILL BE ERASED", self.disk.label())?;
        writeln!(f, "             1: EFI system partition, 2 GiB, FAT32, /efi")?;
        writeln!(f, "             2: btrfs: @deployments (read-only system), @home, @var, @etc-overlay")?;
        writeln!(f, "  Computer:  {}", c.system.hostname)?;
        writeln!(f, "  User:      {} (administrator, password from the config hash)", c.user.name)?;
        writeln!(f, "  Region:    timezone {}, locale {}, keyboard {}", c.system.timezone, c.system.locale, c.system.keymap)?;
        if c.options.desktop {
            writeln!(f, "  Desktop:   KDE Plasma (Wayland)")?;
            writeln!(f, "  Graphics:  {}", self.gpu_summary)?;
            writeln!(f, "             packages: {}", self.packages_extra.join(" "))?;
        } else {
            writeln!(f, "  Desktop:   none (console system)")?;
        }
        writeln!(f, "  Boot:      systemd-boot, one kernel image per system version")?;
        write!(f, "  Source:    {}", c.options.source)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::detect::{Disk, classify_gpus};

    fn inv(uefi: bool) -> Inventory {
        Inventory {
            disks: vec![
                Disk { path: "/dev/vda".into(), size_bytes: 40 << 30, model: "".into(), transport: "virtio".into(), removable: false, in_use: false, install_medium: false },
                Disk { path: "/dev/sdb".into(), size_bytes: 32 << 30, model: "USB".into(), transport: "usb".into(), removable: true, in_use: true, install_medium: true },
            ],
            gpu: classify_gpus(&[(0x1AF4, 0x1050)]),
            uefi,
        }
    }

    fn config(device: &str) -> InstallConfig {
        InstallConfig::from_toml(&format!(
            "[disk]\ndevice = \"{device}\"\nerase = true\n[system]\nhostname = \"lab\"\n\
             [user]\nname = \"test\"\npassword_hash = \"$6$saltsalt$abcdefghijklmnopqrstuvwxyz0123456789\"\n"
        ))
        .unwrap()
    }

    #[test]
    fn graphics_in_plain_words() {
        assert_eq!(gpu_summary(&classify_gpus(&[(0x1AF4, 0x1050)])), "Virtual graphics · standard driver");
        assert_eq!(gpu_summary(&classify_gpus(&[(0x1002, 0x744C)])), "AMD graphics · open Mesa drivers");
        assert_eq!(
            gpu_summary(&classify_gpus(&[(0x8086, 0xA780), (0x10DE, 0x2684)])),
            "Intel graphics + NVIDIA graphics · NVIDIA driver (open modules), switchable"
        );
        assert!(gpu_summary(&classify_gpus(&[(0x10DE, 0x1B80)])).contains("580"));
    }

    #[test]
    fn plan_for_a_good_disk() {
        let p = build(&config("/dev/disk/by-path/x"), &inv(true), "/dev/vda").unwrap();
        let text = p.to_string();
        assert!(text.contains("/dev/vda") && text.contains("ERASED") && text.contains("KDE Plasma"), "{text}");
    }

    #[test]
    fn refuses_the_install_medium_bios_and_unknown_disks() {
        let err = build(&config("/dev/sdb"), &inv(false), "/dev/sdb").unwrap_err().to_string();
        assert!(err.contains("install medium") && err.contains("BIOS"), "{err}");
        let err = build(&config("/dev/sdz"), &inv(true), "/dev/sdz").unwrap_err().to_string();
        assert!(err.contains("not a whole disk"), "{err}");
    }
}
