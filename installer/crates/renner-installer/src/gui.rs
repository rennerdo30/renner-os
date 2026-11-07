//! The graphical installer (Slint). On the plain Arch ISO it draws straight to
//! the screen (LinuxKMS backend); the engine runs on a worker thread and
//! reports progress back to the UI thread.

use anyhow::{Context, Result, anyhow};
use installer_core::config::{DiskConfig, Options, SystemConfig, UserConfig};
use installer_core::detect::Disk;
use installer_core::engine::{self, Event, STAGES};
use installer_core::{InstallConfig, Inventory, Plan, plan};
use slint::{ComponentHandle, ModelRc, SharedString, VecModel};
use std::cell::RefCell;
use std::collections::VecDeque;
use std::io::Write;
use std::process::{Command, Stdio};
use std::rc::Rc;

slint::include_modules!();

/// SHA-512 crypt hash via openssl (on the Arch ISO), so the plain password
/// never leaves this process.
fn hash_password(password: &str) -> Result<String> {
    let mut child = Command::new("openssl")
        .args(["passwd", "-6", "-stdin"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .context("could not run openssl to hash the password")?;
    child.stdin.take().expect("piped").write_all(password.as_bytes())?;
    let out = child.wait_with_output()?;
    anyhow::ensure!(out.status.success(), "openssl could not hash the password");
    Ok(String::from_utf8(out.stdout)?.trim().to_string())
}

fn options_from_env() -> Options {
    let mut options = Options::default();
    if let Ok(source) = std::env::var("RENNER_SOURCE") {
        options.source = source; // lab: the package cache on the host
    }
    options.serial_console = std::env::var("RENNER_SERIAL_CONSOLE").is_ok_and(|v| v == "1");
    options
}

/// Picks a UI scale from the first connected display's preferred mode, so the
/// installer stays readable on 1440p and 4K screens (the KMS backend draws at
/// the native resolution). An explicit SLINT_SCALE_FACTOR wins.
fn set_scale_factor() {
    if std::env::var_os("SLINT_SCALE_FACTOR").is_some() {
        return;
    }
    let height = std::fs::read_dir("/sys/class/drm")
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| std::fs::read_to_string(e.path().join("status")).is_ok_and(|s| s.trim() == "connected"))
        .filter_map(|e| std::fs::read_to_string(e.path().join("modes")).ok())
        .filter_map(|modes| {
            let first = modes.lines().next()?.to_string();
            first.split('x').nth(1)?.trim_end_matches(|c: char| !c.is_ascii_digit()).parse::<u32>().ok()
        })
        .next();
    if let Some(h) = height {
        // 800-1200 px tall: 1.0; 1440: 1.25; 2160 (4K): 2.0.
        let scale = ((h as f32 / 1080.0) * 4.0).floor() / 4.0;
        let scale = scale.clamp(1.0, 3.0);
        // SAFETY: set before any other thread exists (start of the GUI path).
        unsafe { std::env::set_var("SLINT_SCALE_FACTOR", format!("{scale}")) };
    }
}

pub fn run() -> Result<()> {
    set_scale_factor();
    let inventory = Inventory::detect()?;
    let ui = InstallerWindow::new().map_err(|e| anyhow!("could not open the installer window: {e}"))?;

    let disks: Vec<Disk> = inventory.disks.iter().filter(|d| d.usable()).cloned().collect();
    let hidden: Vec<String> = inventory
        .disks
        .iter()
        .filter(|d| !d.usable())
        .map(|d| {
            let why = if d.install_medium { "the install medium" } else if d.in_use { "in use" } else { "smaller than 32 GB" };
            format!("{} ({why})", d.path)
        })
        .collect();
    let rows: Vec<DiskRow> =
        disks.iter().map(|d| DiskRow { title: d.title().into(), detail: d.detail().into() }).collect();
    ui.set_disks(ModelRc::new(VecModel::from(rows)));
    ui.set_disk_note(if hidden.is_empty() { "".into() } else { format!("Not offered: {}", hidden.join(", ")).into() });
    if !disks.is_empty() {
        ui.set_disk_index(0);
    } else {
        ui.set_error_text("No disk of 32 GB or more was found.".into());
    }

    let inventory = Rc::new(inventory);
    let disks = Rc::new(disks);
    let pending: Rc<RefCell<Option<Plan>>> = Rc::new(RefCell::new(None));

    // On-screen keyboard: append to or delete from the targeted field.
    ui.on_osk_typed({
        let weak = ui.as_weak();
        move |target, key| {
            let ui = weak.unwrap();
            let current = match target {
                0 => ui.get_username(),
                1 => ui.get_hostname(),
                2 => ui.get_password(),
                _ => ui.get_password2(),
            };
            let mut text = current.to_string();
            if key == "\u{8}" {
                text.pop();
            } else {
                text.push_str(&key);
            }
            let text: SharedString = text.into();
            match target {
                0 => ui.set_username(text),
                1 => ui.set_hostname(text),
                2 => ui.set_password(text),
                _ => ui.set_password2(text),
            }
        }
    });

    ui.on_make_plan({
        let weak = ui.as_weak();
        let (inventory, disks, pending) = (inventory.clone(), disks.clone(), pending.clone());
        move || {
            let ui = weak.unwrap();
            let result = (|| -> Result<Plan> {
                let disk = usize::try_from(ui.get_disk_index())
                    .ok()
                    .and_then(|i| disks.get(i))
                    .ok_or_else(|| anyhow!("Choose a disk first."))?;
                let (pw, pw2) = (ui.get_password().to_string(), ui.get_password2().to_string());
                anyhow::ensure!(!ui.get_username().is_empty(), "Enter a user name.");
                anyhow::ensure!(!pw.is_empty(), "Enter a password.");
                anyhow::ensure!(pw == pw2, "The two passwords are not the same.");
                let config = InstallConfig {
                    disk: DiskConfig { device: disk.path.clone(), erase: true },
                    system: SystemConfig {
                        hostname: ui.get_hostname().to_string(),
                        timezone: "UTC".into(),
                        locale: "en_US.UTF-8".into(),
                        keymap: "us".into(),
                    },
                    user: UserConfig { name: ui.get_username().to_string(), password_hash: hash_password(&pw)? },
                    options: options_from_env(),
                };
                plan::build(&config, &inventory, &disk.path)
            })();
            match result {
                Ok(plan) => {
                    let rows: Vec<PlanRow> =
                        plan.rows().into_iter().map(|(l, v)| PlanRow { label: l.into(), value: v.into() }).collect();
                    ui.set_plan_rows(ModelRc::new(VecModel::from(rows)));
                    ui.set_erase_warning(plan.erase_warning().into());
                    ui.set_error_text("".into());
                    *pending.borrow_mut() = Some(plan);
                    ui.set_page(3);
                }
                Err(e) => ui.set_error_text(format!("{e:#}").into()),
            }
        }
    });

    ui.on_start_install({
        let weak = ui.as_weak();
        let pending = pending.clone();
        move || {
            let Some(plan) = pending.borrow_mut().take() else { return };
            let ui = weak.unwrap();
            ui.set_page(4);
            ui.set_error_text("".into());
            ui.set_stage_text("Getting started".into());
            let weak = ui.as_weak();
            std::thread::spawn(move || {
                let mut tail: VecDeque<String> = VecDeque::new();
                let result = engine::run(&plan, |event| {
                    let (stage, progress) = match &event {
                        Event::Stage(i, text) => (Some(text.clone()), Some((*i + 1) as f32 / STAGES.len() as f32)),
                        Event::Line(line) => {
                            tail.push_back(line.clone());
                            if tail.len() > 14 {
                                tail.pop_front();
                            }
                            (None, None)
                        }
                    };
                    let log = tail.iter().cloned().collect::<Vec<_>>().join("\n");
                    let weak = weak.clone();
                    let _ = slint::invoke_from_event_loop(move || {
                        let ui = weak.unwrap();
                        if let Some(s) = stage {
                            ui.set_stage_text(s.into());
                        }
                        if let Some(p) = progress {
                            ui.set_progress(p);
                        }
                        ui.set_log_tail(log.into());
                    });
                });
                let weak = weak.clone();
                let _ = slint::invoke_from_event_loop(move || {
                    let ui = weak.unwrap();
                    match result {
                        Ok(()) => {
                            ui.set_progress(1.0);
                            ui.set_page(5);
                        }
                        Err(e) => {
                            ui.set_error_text(format!("{e:#}").into());
                            ui.set_page(6);
                        }
                    }
                });
            });
        }
    });

    ui.on_reboot(|| {
        let _ = Command::new("systemctl").arg("reboot").status();
    });

    ui.run().map_err(|e| anyhow!("installer window failed: {e}"))
}
