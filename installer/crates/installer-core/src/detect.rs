//! Hardware detection. Every parser takes its raw input as a parameter, so it
//! is unit-tested with recorded data on any OS; `Inventory::detect` reads the
//! real machine (Linux only).

use anyhow::{Context, Result};
use serde::Deserialize;
use std::path::Path;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Disk {
    pub path: String,
    pub size_bytes: u64,
    pub model: String,
    pub transport: String,
    pub removable: bool,
    /// Mounted somewhere (itself or a partition). Never offered.
    pub in_use: bool,
    /// The medium the installer runs from (Arch ISO stick). Never offered.
    pub install_medium: bool,
}

impl Disk {
    pub fn size_gib(&self) -> f64 {
        self.size_bytes as f64 / (1u64 << 30) as f64
    }
    /// What kind of disk this is, in plain words.
    pub fn kind(&self) -> &'static str {
        match self.transport.as_str() {
            "nvme" => "NVMe SSD",
            "usb" => "USB drive",
            "sata" | "ata" => "SATA disk",
            "virtio" => "Virtual disk",
            "mmc" => "Memory card",
            _ => "Disk",
        }
    }
    /// Size the way disks are sold (decimal GB/TB).
    pub fn size_text(&self) -> String {
        let gb = self.size_bytes as f64 / 1e9;
        if gb >= 1000.0 { format!("{:.1} TB", gb / 1000.0) } else { format!("{gb:.0} GB") }
    }
    /// One line, e.g. "Samsung SSD 990 PRO · 2.0 TB" or "Virtual disk · 43 GB".
    pub fn title(&self) -> String {
        let name = if self.model.is_empty() { self.kind() } else { &self.model };
        format!("{name} · {}", self.size_text())
    }
    /// Second line, e.g. "NVMe SSD · /dev/nvme0n1".
    pub fn detail(&self) -> String {
        format!("{} · {}", self.kind(), self.path)
    }
    pub fn label(&self) -> String {
        format!("{} ({})", self.title(), self.path)
    }
    pub fn usable(&self) -> bool {
        !self.in_use && !self.install_medium && self.size_bytes >= crate::plan::MIN_DISK_BYTES
    }
}

#[derive(Deserialize)]
struct LsblkRoot {
    blockdevices: Vec<LsblkDev>,
}

#[derive(Deserialize)]
struct LsblkDev {
    path: Option<String>,
    size: Option<u64>,
    model: Option<String>,
    #[serde(rename = "type")]
    kind: Option<String>,
    tran: Option<String>,
    #[serde(default)]
    rm: bool,
    #[serde(default)]
    ro: bool,
    #[serde(default)]
    mountpoints: Vec<Option<String>>,
    #[serde(default)]
    children: Vec<LsblkDev>,
}

impl LsblkDev {
    fn mounts(&self) -> Vec<String> {
        let mut all: Vec<String> = self.mountpoints.iter().flatten().cloned().collect();
        for child in &self.children {
            all.extend(child.mounts());
        }
        all
    }
}

/// Parses `lsblk -J -b -o PATH,SIZE,MODEL,TYPE,TRAN,RM,RO,MOUNTPOINTS`.
pub fn parse_lsblk(json: &str) -> Result<Vec<Disk>> {
    let root: LsblkRoot = serde_json::from_str(json).context("lsblk output is not valid JSON")?;
    Ok(root
        .blockdevices
        .into_iter()
        .filter(|d| d.kind.as_deref() == Some("disk") && !d.ro)
        .filter_map(|d| {
            let mounts = d.mounts();
            Some(Disk {
                path: d.path.clone()?,
                size_bytes: d.size.unwrap_or(0),
                model: d.model.clone().unwrap_or_default().trim().to_string(),
                transport: d.tran.clone().unwrap_or_default(),
                removable: d.rm,
                in_use: !mounts.is_empty(),
                install_medium: mounts.iter().any(|m| m.starts_with("/run/archiso")),
            })
        })
        .collect())
}

// --- GPUs (same rules as system/usr/bin/renner-gpu-detect) -----------------

const NVIDIA: u32 = 0x10DE;
const NVIDIA_TURING_FIRST: u32 = 0x1E00;
const NVIDIA_MAXWELL_FIRST: u32 = 0x1340;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Gpu {
    pub vendor_id: u32,
    pub device_id: u32,
    pub vendor: &'static str,
    pub driver: &'static str,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GpuProfile {
    pub gpus: Vec<Gpu>,
    /// open | legacy-580 | nouveau | none
    pub nvidia_driver: &'static str,
    pub packages: Vec<&'static str>,
}

pub fn classify_gpus(ids: &[(u32, u32)]) -> GpuProfile {
    let gpus: Vec<Gpu> = ids
        .iter()
        .map(|&(vendor_id, device_id)| {
            let (vendor, driver) = match vendor_id {
                0x1002 => ("amd", "amd"),
                0x8086 => ("intel", "intel"),
                NVIDIA if device_id >= NVIDIA_TURING_FIRST => ("nvidia", "open"),
                NVIDIA if device_id >= NVIDIA_MAXWELL_FIRST => ("nvidia", "legacy-580"),
                NVIDIA => ("nvidia", "nouveau"),
                _ => ("other", "mesa"),
            };
            Gpu { vendor_id, device_id, vendor, driver }
        })
        .collect();

    let nv: Vec<&str> = gpus.iter().filter(|g| g.vendor == "nvidia").map(|g| g.driver).collect();
    let nvidia_driver = if nv.contains(&"legacy-580") {
        "legacy-580"
    } else if nv.contains(&"open") {
        "open"
    } else if !nv.is_empty() {
        "nouveau"
    } else {
        "none"
    };

    let mut packages = vec!["mesa", "lib32-mesa", "vulkan-icd-loader", "lib32-vulkan-icd-loader"];
    let mut drivers: Vec<&str> = gpus.iter().filter(|g| g.vendor != "nvidia").map(|g| g.driver).collect();
    if nvidia_driver != "none" {
        drivers.push(nvidia_driver);
    }
    drivers.sort();
    drivers.dedup();
    for d in drivers {
        packages.extend(match d {
            "amd" => &["vulkan-radeon", "lib32-vulkan-radeon"][..],
            "intel" => &["vulkan-intel", "lib32-vulkan-intel", "intel-media-driver"][..],
            "open" => &["nvidia-open", "nvidia-utils", "lib32-nvidia-utils", "nvidia-settings"][..],
            "nouveau" | "legacy-580" => &["vulkan-nouveau", "lib32-vulkan-nouveau"][..],
            _ => &[][..],
        });
    }
    if gpus.len() > 1 {
        packages.push("switcheroo-control");
        if nvidia_driver == "open" && gpus.iter().any(|g| g.vendor != "nvidia") {
            packages.push("nvidia-prime");
        }
    }
    let mut seen = std::collections::HashSet::new();
    packages.retain(|p| seen.insert(*p));
    GpuProfile { gpus, nvidia_driver, packages }
}

/// Reads display controllers (PCI class 0x03) from a sysfs PCI device directory.
pub fn scan_gpu_ids(sysfs: &Path) -> Vec<(u32, u32)> {
    let read_hex = |p: &Path| -> Option<u32> {
        let s = std::fs::read_to_string(p).ok()?;
        u32::from_str_radix(s.trim().trim_start_matches("0x"), 16).ok()
    };
    let mut entries: Vec<_> = std::fs::read_dir(sysfs).into_iter().flatten().flatten().collect();
    entries.sort_by_key(|e| e.file_name());
    entries
        .iter()
        .filter_map(|e| {
            let dir = e.path();
            let class = read_hex(&dir.join("class"))?;
            (class >> 16 == 0x03).then_some(())?;
            Some((read_hex(&dir.join("vendor"))?, read_hex(&dir.join("device"))?))
        })
        .collect()
}

// --- the machine ------------------------------------------------------------

#[derive(Debug, Clone)]
pub struct Inventory {
    pub disks: Vec<Disk>,
    pub gpu: GpuProfile,
    pub uefi: bool,
}

impl Inventory {
    /// Reads the real machine. Linux only (lsblk, sysfs).
    pub fn detect() -> Result<Self> {
        let out = std::process::Command::new("lsblk")
            .args(["-J", "-b", "-o", "PATH,SIZE,MODEL,TYPE,TRAN,RM,RO,MOUNTPOINTS"])
            .output()
            .context("could not run lsblk")?;
        anyhow::ensure!(out.status.success(), "lsblk failed: {}", String::from_utf8_lossy(&out.stderr));
        Ok(Self {
            disks: parse_lsblk(&String::from_utf8_lossy(&out.stdout))?,
            gpu: classify_gpus(&scan_gpu_ids(Path::new("/sys/bus/pci/devices"))),
            uefi: Path::new("/sys/firmware/efi/efivars").is_dir(),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Recorded from the lab VM booted from the Arch ISO (trimmed).
    const LSBLK: &str = r#"{"blockdevices":[
      {"path":"/dev/loop0","size":890830848,"model":null,"type":"loop","tran":null,"rm":false,"ro":true,"mountpoints":["/run/archiso/airootfs"]},
      {"path":"/dev/sr0","size":1431306240,"model":"QEMU DVD-ROM","type":"rom","tran":"ata","rm":true,"ro":false,"mountpoints":["/run/archiso/bootmnt"]},
      {"path":"/dev/sdb","size":32017047552,"model":"USB Stick","type":"disk","tran":"usb","rm":true,"ro":false,"mountpoints":[null],
       "children":[{"path":"/dev/sdb1","size":1431306240,"model":null,"type":"part","tran":null,"rm":true,"ro":false,"mountpoints":["/run/archiso/bootmnt"]}]},
      {"path":"/dev/vda","size":42949672960,"model":null,"type":"disk","tran":"virtio","rm":false,"ro":false,"mountpoints":[null]},
      {"path":"/dev/nvme0n1","size":16106127360,"model":"Small NVMe ","type":"disk","tran":"nvme","rm":false,"ro":false,"mountpoints":[null]}
    ]}"#;

    #[test]
    fn lsblk_finds_disks_and_flags_the_install_medium() {
        let disks = parse_lsblk(LSBLK).unwrap();
        let paths: Vec<_> = disks.iter().map(|d| d.path.as_str()).collect();
        assert_eq!(paths, ["/dev/sdb", "/dev/vda", "/dev/nvme0n1"]); // no loop, no rom
        let stick = &disks[0];
        assert!(stick.in_use && stick.install_medium && !stick.usable());
        assert!(disks[1].usable());
        assert!(!disks[2].usable(), "15 GiB is below the minimum");
        assert_eq!(disks[2].model, "Small NVMe");
        assert_eq!(disks[1].title(), "Virtual disk · 43 GB");
        assert_eq!(disks[2].detail(), "NVMe SSD · /dev/nvme0n1");
    }

    #[test]
    fn gpu_rules_match_the_python_detector() {
        let p = classify_gpus(&[(0x10DE, 0x2684)]); // RTX 4090
        assert_eq!(p.nvidia_driver, "open");
        assert!(p.packages.contains(&"nvidia-open"));
        let p = classify_gpus(&[(0x10DE, 0x1F82)]); // GTX 1650, first open generation
        assert_eq!(p.nvidia_driver, "open");
        let p = classify_gpus(&[(0x10DE, 0x1B80)]); // GTX 1080
        assert_eq!(p.nvidia_driver, "legacy-580");
        assert!(!p.packages.contains(&"nvidia-open"));
        let p = classify_gpus(&[(0x10DE, 0x1180)]); // GTX 680
        assert_eq!(p.nvidia_driver, "nouveau");
        let p = classify_gpus(&[(0x8086, 0xA780), (0x10DE, 0x2684)]); // hybrid laptop
        for pkg in ["vulkan-intel", "nvidia-open", "switcheroo-control", "nvidia-prime"] {
            assert!(p.packages.contains(&pkg), "{pkg}");
        }
        let p = classify_gpus(&[(0x1AF4, 0x1050)]); // virtio
        assert_eq!(p.gpus[0].vendor, "other");
        assert_eq!(p.packages[0], "mesa");
    }
}
