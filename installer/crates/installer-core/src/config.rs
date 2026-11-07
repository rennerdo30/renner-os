//! The install configuration: what the GUI collects and what an unattended
//! install reads from a TOML file. Both paths go through `validate`.

use anyhow::{Result, bail};
use serde::{Deserialize, Serialize};

/// Where the installer fetches `system/` files when nothing else is given.
pub const DEFAULT_SOURCE: &str = "https://raw.githubusercontent.com/rennerdo30/renner-os/main";

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct InstallConfig {
    pub disk: DiskConfig,
    pub system: SystemConfig,
    pub user: UserConfig,
    #[serde(default)]
    pub options: Options,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct DiskConfig {
    /// Whole disk, e.g. `/dev/nvme0n1` or a `/dev/disk/by-id/...` alias.
    pub device: String,
    /// Must be `true`: the whole disk is erased. Guards against a config
    /// written for another machine.
    pub erase: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SystemConfig {
    pub hostname: String,
    #[serde(default = "default_timezone")]
    pub timezone: String,
    #[serde(default = "default_locale")]
    pub locale: String,
    #[serde(default = "default_keymap")]
    pub keymap: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct UserConfig {
    pub name: String,
    /// A crypt(3) hash (`openssl passwd -6`); plain passwords never go into
    /// a config file.
    pub password_hash: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Options {
    /// Install KDE Plasma and the detected GPU drivers.
    #[serde(default = "yes")]
    pub desktop: bool,
    /// Kernel console on ttyS0 too (test VMs).
    #[serde(default)]
    pub serial_console: bool,
    /// Where `system/` files come from.
    #[serde(default = "default_source")]
    pub source: String,
}

impl Default for Options {
    fn default() -> Self {
        Self { desktop: true, serial_console: false, source: default_source() }
    }
}

fn default_timezone() -> String { "UTC".into() }
fn default_locale() -> String { "en_US.UTF-8".into() }
fn default_keymap() -> String { "us".into() }
fn default_source() -> String { DEFAULT_SOURCE.into() }
fn yes() -> bool { true }

impl InstallConfig {
    pub fn from_toml(text: &str) -> Result<Self> {
        let config: InstallConfig = toml::from_str(text)?;
        config.validate()?;
        Ok(config)
    }

    /// Checks everything that can be checked without the target machine.
    /// Machine-specific checks (does the disk exist, is it a whole disk)
    /// happen in `plan::build`.
    pub fn validate(&self) -> Result<()> {
        let mut problems = Vec::new();
        if !self.disk.erase {
            problems.push("disk.erase must be true: the whole disk is erased".to_string());
        }
        if !self.disk.device.starts_with("/dev/") {
            problems.push(format!("disk.device '{}' is not a /dev path", self.disk.device));
        }
        if !valid_hostname(&self.system.hostname) {
            problems.push(format!(
                "system.hostname '{}' must be 1-63 letters, digits or hyphens, not starting or ending with a hyphen",
                self.system.hostname
            ));
        }
        if !valid_username(&self.user.name) {
            problems.push(format!(
                "user.name '{}' must start with a lowercase letter or _, then lowercase letters, digits, _ or -",
                self.user.name
            ));
        }
        if !valid_crypt_hash(&self.user.password_hash) {
            problems.push(
                "user.password_hash must be a crypt hash such as the output of `openssl passwd -6` \
                 (starting with $6$, $5$ or $y$)"
                    .to_string(),
            );
        }
        if self.system.locale.is_empty() || self.system.keymap.is_empty() || self.system.timezone.is_empty() {
            problems.push("system.locale, system.keymap and system.timezone must not be empty".to_string());
        }
        // file:/// is the install medium itself (the patched ISO carries install/ and system/).
        let source = &self.options.source;
        if !(source.starts_with("https://") || source.starts_with("http://") || source.starts_with("file:///")) {
            problems.push(format!("options.source '{source}' must be an http(s) URL or a file:/// path"));
        }
        if problems.is_empty() {
            Ok(())
        } else {
            bail!("invalid install config:\n  - {}", problems.join("\n  - "))
        }
    }
}

pub fn valid_hostname(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 63
        && !name.starts_with('-')
        && !name.ends_with('-')
        && name.chars().all(|c| c.is_ascii_alphanumeric() || c == '-')
}

pub fn valid_username(name: &str) -> bool {
    let mut chars = name.chars();
    match chars.next() {
        Some(c) if c.is_ascii_lowercase() || c == '_' => {}
        _ => return false,
    }
    name.len() <= 32 && chars.all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_' || c == '-')
}

fn valid_crypt_hash(hash: &str) -> bool {
    ["$6$", "$5$", "$y$"].iter().any(|p| hash.starts_with(p))
        && hash.len() > 20
        && !hash.contains(char::is_whitespace)
        && !hash.contains(':')
}

#[cfg(test)]
mod tests {
    use super::*;

    const GOOD: &str = r#"
        [disk]
        device = "/dev/vda"
        erase = true

        [system]
        hostname = "renner-lab"
        timezone = "Europe/Berlin"

        [user]
        name = "test"
        password_hash = "$6$saltsalt$abcdefghijklmnopqrstuvwxyz0123456789"
    "#;

    #[test]
    fn good_config_parses_with_defaults() {
        let c = InstallConfig::from_toml(GOOD).unwrap();
        assert_eq!(c.system.locale, "en_US.UTF-8");
        assert_eq!(c.system.keymap, "us");
        assert!(c.options.desktop);
        assert!(!c.options.serial_console);
        assert_eq!(c.options.source, DEFAULT_SOURCE);
    }

    #[test]
    fn erase_must_be_explicit() {
        let text = GOOD.replace("erase = true", "erase = false");
        let err = InstallConfig::from_toml(&text).unwrap_err().to_string();
        assert!(err.contains("disk.erase"), "{err}");
    }

    #[test]
    fn plain_password_is_refused() {
        let text = GOOD.replace("$6$saltsalt$abcdefghijklmnopqrstuvwxyz0123456789", "hunter2");
        let err = InstallConfig::from_toml(&text).unwrap_err().to_string();
        assert!(err.contains("password_hash"), "{err}");
    }

    #[test]
    fn unknown_keys_are_refused() {
        let text = format!("{GOOD}\n[extra]\nfoo = 1\n");
        assert!(InstallConfig::from_toml(&text).is_err());
    }

    #[test]
    fn all_problems_are_reported_at_once() {
        let text = GOOD
            .replace("renner-lab", "-bad-")
            .replace("name = \"test\"", "name = \"Test\"");
        let err = InstallConfig::from_toml(&text).unwrap_err().to_string();
        assert!(err.contains("hostname") && err.contains("user.name"), "{err}");
    }

    #[test]
    fn source_may_be_the_install_medium() {
        let with = |s: &str| format!("{GOOD}\n[options]\nsource = \"{s}\"\n");
        assert!(InstallConfig::from_toml(&with("file:///run/archiso/bootmnt/renner/source")).is_ok());
        assert!(InstallConfig::from_toml(&with("http://10.0.2.2:8079/renner")).is_ok());
        for bad in ["file://host/x", "ftp://mirror/x", "/run/archiso"] {
            let err = InstallConfig::from_toml(&with(bad)).unwrap_err().to_string();
            assert!(err.contains("options.source"), "{bad}: {err}");
        }
    }

    #[test]
    fn names() {
        assert!(valid_hostname("renner"));
        assert!(!valid_hostname("renner.local"));
        assert!(valid_username("_svc-1"));
        assert!(!valid_username("1abc"));
    }
}
