//! Network page: is the machine online, and Wi-Fi through iwd (`iwctl`), which
//! the Arch ISO uses. Cables need nothing: the ISO configures them by itself.

use std::process::Command;
use std::time::{Duration, Instant};

pub struct Wifi {
    pub name: String,
    pub secured: bool,
    /// 0-4 bars.
    pub signal: usize,
}

/// Online = a default route and a name that resolves.
pub fn online() -> bool {
    let route = Command::new("ip").args(["route", "show", "default"]).output();
    let has_route = route.is_ok_and(|o| !o.stdout.is_empty());
    has_route
        && Command::new("getent")
            .args(["ahosts", &probe_host()])
            .output()
            .is_ok_and(|o| o.status.success())
}

/// The host the install will talk to first: the pacman mirror.
fn probe_host() -> String {
    let mirrorlist = std::fs::read_to_string("/etc/pacman.d/mirrorlist").unwrap_or_default();
    mirrorlist
        .lines()
        .filter_map(|l| l.trim().strip_prefix("Server"))
        .filter_map(|l| l.split("://").nth(1))
        .filter_map(|rest| rest.split(['/', ':']).next())
        .next()
        .unwrap_or("archlinux.org")
        .to_string()
}

/// True if a cable is plugged in (a wired interface with carrier).
pub fn cable() -> bool {
    interfaces().iter().any(|(name, wireless)| {
        !wireless
            && name != "lo"
            && std::fs::read_to_string(format!("/sys/class/net/{name}/carrier")).is_ok_and(|c| c.trim() == "1")
    })
}

/// The first wireless interface, if any.
pub fn wifi_device() -> Option<String> {
    interfaces().into_iter().find(|(_, wireless)| *wireless).map(|(name, _)| name)
}

fn interfaces() -> Vec<(String, bool)> {
    std::fs::read_dir("/sys/class/net")
        .into_iter()
        .flatten()
        .flatten()
        .map(|e| {
            let name = e.file_name().to_string_lossy().to_string();
            let wireless = e.path().join("wireless").exists() || e.path().join("phy80211").exists();
            (name, wireless)
        })
        .collect()
}

/// Scans and lists the networks in range, strongest first.
pub fn scan(device: &str) -> Vec<Wifi> {
    let _ = Command::new("iwctl").args(["station", device, "scan"]).status();
    std::thread::sleep(Duration::from_secs(3));
    let out = Command::new("iwctl").args(["station", device, "get-networks"]).output();
    let text = out.map(|o| String::from_utf8_lossy(&o.stdout).to_string()).unwrap_or_default();
    let mut list: Vec<Wifi> = strip_ansi(&text).lines().filter_map(parse_network).collect();
    list.sort_by(|a, b| b.signal.cmp(&a.signal));
    list.dedup_by(|a, b| a.name == b.name);
    list
}

/// One row of `iwctl station X get-networks`:
/// `  > MyNet                 psk                 ****`
fn parse_network(line: &str) -> Option<Wifi> {
    let line = line.trim().trim_start_matches('>').trim();
    let stars = line.chars().rev().take_while(|c| *c == '*').count();
    let rest = line[..line.len() - stars].trim_end();
    let (name, security) = rest.rsplit_once(char::is_whitespace)?;
    let name = name.trim();
    if name.is_empty() || !["psk", "open", "8021x", "wep", "owe"].contains(&security) {
        return None;
    }
    Some(Wifi { name: name.to_string(), secured: security != "open", signal: stars.min(4) })
}

fn strip_ansi(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut chars = text.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '\u{1b}' {
            // ESC [ ... final letter
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

/// Joins a network and waits up to 20 s until the machine is online. The
/// network is remembered for the new system (/tmp/renner-wifi, read by
/// install.sh).
pub fn connect(device: &str, name: &str, password: &str) -> Result<(), String> {
    let mut cmd = Command::new("iwctl");
    if !password.is_empty() {
        cmd.args(["--passphrase", password]);
    }
    let status = cmd.args(["station", device, "connect", name]).output().map_err(|e| e.to_string())?;
    if !status.status.success() {
        let msg = strip_ansi(&String::from_utf8_lossy(&status.stdout)).trim().to_string();
        return Err(if msg.is_empty() { "Could not connect. Check the password.".into() } else { msg });
    }
    let start = Instant::now();
    while start.elapsed() < Duration::from_secs(20) {
        if online() {
            use std::os::unix::fs::OpenOptionsExt;
            use std::io::Write;
            if let Ok(mut f) =
                std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open("/tmp/renner-wifi")
            {
                let _ = writeln!(f, "{name}\n{password}");
            }
            return Ok(());
        }
        std::thread::sleep(Duration::from_millis(700));
    }
    Err("Connected to the network, but there is no internet.".into())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_iwctl_rows() {
        let text = "\x1b[1;90m  Network name   Security   Signal\x1b[0m\n  \x1b[0m> Home Net\x1b[0m        psk     ****\n  Cafe   open   **\n";
        let rows: Vec<Wifi> = strip_ansi(text).lines().filter_map(parse_network).collect();
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0].name, "Home Net");
        assert!(rows[0].secured);
        assert_eq!(rows[0].signal, 4);
        assert_eq!(rows[1].name, "Cafe");
        assert!(!rows[1].secured);
    }
}
