//! The graphical installer (Slint). On the plain Arch ISO it draws straight to
//! the screen (LinuxKMS backend); the engine runs on a worker thread and
//! reports progress back to the UI thread.

use anyhow::{Context, Result, anyhow};
use installer_core::config::{DiskConfig, Options, SystemConfig, UserConfig};
use installer_core::detect::Disk;
use installer_core::engine::{self, Event, STAGES};
use installer_core::{InstallConfig, Inventory, Plan, plan};
use slint::{ComponentHandle, ModelRc, SharedString, StandardListViewItem, VecModel};
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

pub fn run() -> Result<()> {
    let inventory = Inventory::detect()?;
    let ui = InstallerWindow::new().map_err(|e| anyhow!("could not open the installer window: {e}"))?;

    let disks: Vec<Disk> = inventory.disks.iter().filter(|d| d.usable()).cloned().collect();
    let hidden: Vec<String> = inventory
        .disks
        .iter()
        .filter(|d| !d.usable())
        .map(|d| {
            let why = if d.install_medium { "install medium" } else if d.in_use { "in use" } else { "smaller than 32 GB" };
            format!("{} ({why})", d.path)
        })
        .collect();
    let labels: Vec<StandardListViewItem> =
        disks.iter().map(|d| StandardListViewItem::from(SharedString::from(d.label()))).collect();
    ui.set_disk_labels(ModelRc::new(VecModel::from(labels)));
    ui.set_disk_note(if hidden.is_empty() { "".into() } else { format!("Not offered: {}", hidden.join(", ")).into() });
    if disks.len() == 1 {
        ui.set_disk_index(0);
    }

    let inventory = Rc::new(inventory);
    let disks = Rc::new(disks);
    let pending: Rc<RefCell<Option<Plan>>> = Rc::new(RefCell::new(None));

    ui.on_make_plan({
        let weak = ui.as_weak();
        let (inventory, disks, pending) = (inventory.clone(), disks.clone(), pending.clone());
        move || {
            let ui = weak.unwrap();
            let result = (|| -> Result<Plan> {
                let disk = usize::try_from(ui.get_disk_index())
                    .ok()
                    .and_then(|i| disks.get(i))
                    .ok_or_else(|| anyhow!("choose a disk first"))?;
                let (pw, pw2) = (ui.get_password().to_string(), ui.get_password2().to_string());
                anyhow::ensure!(!pw.is_empty(), "enter a password");
                anyhow::ensure!(pw == pw2, "the passwords do not match");
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
                    ui.set_plan_text(plan.to_string().into());
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
            let weak = ui.as_weak();
            std::thread::spawn(move || {
                let mut tail: VecDeque<String> = VecDeque::new();
                let result = engine::run(&plan, |event| {
                    let (stage, progress) = match &event {
                        Event::Stage(i, text) => (Some(text.clone()), Some((*i + 1) as f32 / STAGES.len() as f32)),
                        Event::Line(line) => {
                            tail.push_back(line.clone());
                            if tail.len() > 16 {
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
