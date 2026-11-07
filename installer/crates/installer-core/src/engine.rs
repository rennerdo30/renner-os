//! Runs an install plan and reports progress.
//!
//! PoC backend: the proven `install/install.sh` stages, embedded in the binary
//! and driven non-interactively. Each `==> ` stage line becomes a progress
//! event. The stages move into Rust one by one, each with the same lab tests.

use crate::plan::Plan;
use anyhow::{Context, Result, bail};
use std::io::{BufRead, BufReader};
use std::process::{Command, Stdio};

pub const INSTALL_SH: &str = include_str!("../../../../install/install.sh");

/// The stages `install.sh` announces, in order (prefix of the `==> ` line).
pub const STAGES: &[&str] = &[
    "Checking the environment",
    "Summary",
    "Fetching renner.dev OS files",
    "Detecting graphics hardware",
    "Partitioning",
    "Creating btrfs subvolumes",
    "Installing Arch Linux packages",
    "Installing the desktop",
    "Installing renner.dev OS components",
    "Configuring the system",
    "Setting up the boot loader",
    "Finishing",
    "renner.dev OS is installed",
];

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
    /// A new stage started: (index into STAGES, its text).
    Stage(usize, String),
    /// Any output line, for the log view.
    Line(String),
}

/// Removes ANSI colour codes from a line of installer output.
pub fn strip_ansi(line: &str) -> String {
    let mut out = String::with_capacity(line.len());
    let mut chars = line.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '\u{1b}' {
            if chars.peek() == Some(&'[') {
                chars.next();
                for c in chars.by_ref() {
                    if c.is_ascii_alphabetic() {
                        break;
                    }
                }
            }
        } else {
            out.push(c);
        }
    }
    out
}

/// Maps an output line to a stage, if it announces one.
pub fn stage_of(line: &str) -> Option<usize> {
    let text = strip_ansi(line);
    let text = text.trim().strip_prefix("==> ")?;
    STAGES.iter().position(|s| text.starts_with(s))
}

/// The arguments for `install.sh`; the password only as a hash.
pub fn script_args(plan: &Plan) -> Vec<String> {
    let c = &plan.config;
    let mut args: Vec<String> = [
        ("--disk", c.disk.device.as_str()),
        ("--hostname", &c.system.hostname),
        ("--user", &c.user.name),
        ("--password-hash", &c.user.password_hash),
        ("--timezone", &c.system.timezone),
        ("--locale", &c.system.locale),
        ("--keymap", &c.system.keymap),
        ("--source", &c.options.source),
    ]
    .iter()
    .flat_map(|(k, v)| [k.to_string(), v.to_string()])
    .collect();
    if c.options.serial_console {
        args.push("--serial-console".into());
    }
    if !c.options.desktop {
        args.push("--no-desktop".into());
    }
    args.push("--yes".into());
    args
}

/// Runs the install. Blocks until it ends; `on_event` sees every stage and line.
pub fn run(plan: &Plan, mut on_event: impl FnMut(Event)) -> Result<()> {
    let dir = std::env::temp_dir().join("renner-installer");
    std::fs::create_dir_all(&dir)?;
    let script = dir.join("install.sh");
    std::fs::write(&script, INSTALL_SH).context("could not write install.sh")?;

    let mut child = Command::new("bash")
        .arg(&script)
        .args(script_args(plan))
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit()) // install.sh sends stderr into its own log pipeline
        .spawn()
        .context("could not start install.sh")?;
    let stdout = child.stdout.take().expect("piped");
    for line in BufReader::new(stdout).lines() {
        let line = line.unwrap_or_default();
        if let Some(i) = stage_of(&line) {
            on_event(Event::Stage(i, STAGES[i].to_string()));
        }
        on_event(Event::Line(strip_ansi(&line)));
    }
    let status = child.wait()?;
    if !status.success() {
        bail!("the install failed (exit {}); log: /tmp/renner-install.log", status.code().unwrap_or(-1));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stages_are_recognised_with_colour_codes() {
        assert_eq!(stage_of("\u{1b}[1m==> Partitioning /dev/vda\u{1b}[0m"), Some(4));
        assert_eq!(stage_of("==> Installing the desktop and graphics drivers (this takes a while)"), Some(7));
        assert_eq!(stage_of("==> Starting build: '7.2.14-arch1-1'"), None); // mkinitcpio, not ours
        assert_eq!(stage_of("plain line"), None);
    }

    #[test]
    fn every_stage_is_announced_by_install_sh() {
        for stage in STAGES {
            assert!(INSTALL_SH.contains(&format!("say \"{stage}")), "install.sh never says '{stage}'");
        }
    }

    #[test]
    fn script_gets_the_hash_never_a_plain_password() {
        let toml = "[disk]\ndevice = \"/dev/vda\"\nerase = true\n[system]\nhostname = \"lab\"\n\
                    [user]\nname = \"test\"\npassword_hash = \"$6$saltsalt$abcdefghijklmnopqrstuvwxyz0123456789\"\n";
        let config = crate::config::InstallConfig::from_toml(toml).unwrap();
        let plan = Plan { config, disk: crate::detect::parse_lsblk(r#"{"blockdevices":[]}"#).unwrap().into_iter().next().unwrap_or(
            crate::detect::Disk { path: "/dev/vda".into(), size_bytes: 40 << 30, model: String::new(), transport: String::new(), removable: false, in_use: false, install_medium: false }),
            gpu_summary: String::new(), packages_extra: vec![] };
        let args = script_args(&plan);
        assert!(args.windows(2).any(|w| w[0] == "--password-hash" && w[1].starts_with("$6$")));
        assert!(!args.iter().any(|a| a == "--password"));
        assert_eq!(args.last().map(String::as_str), Some("--yes"));
    }
}
