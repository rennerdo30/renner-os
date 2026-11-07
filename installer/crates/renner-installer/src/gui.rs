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
use std::sync::{Arc, Mutex};

use crate::{network, region};

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
    // The console underneath must not see our keystrokes (RO-30); restored
    // when this guard drops at the end of run().
    crate::console::ignore_terminal_signals();
    let _console = crate::console::ConsoleGuard::engage();
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

    // Language, keyboard, time zone. A language suggests a keyboard and a zone.
    let zones = Rc::new(region::timezones());
    let strings = |v: Vec<String>| ModelRc::new(VecModel::from(v.into_iter().map(SharedString::from).collect::<Vec<_>>()));
    ui.set_languages(strings(region::LANGUAGES.iter().map(|l| l.label.to_string()).collect()));
    ui.set_keyboards(strings(region::KEYBOARDS.iter().map(|k| k.label.to_string()).collect()));
    ui.set_tz_regions(strings(zones.iter().map(|(r, _)| r.clone()).collect()));
    let show_zone = {
        let weak = ui.as_weak();
        let zones = zones.clone();
        move |zone: &str| {
            let ui = weak.unwrap();
            let (r, c) = region::find_zone(&zones, zone);
            ui.set_tz_region_index(r as i32);
            ui.set_tz_cities(strings(zones[r].1.clone()));
            ui.set_tz_city_index(c as i32);
        }
    };
    let pick_language = {
        let weak = ui.as_weak();
        let show_zone = show_zone.clone();
        move |i: i32| {
            let ui = weak.unwrap();
            let lang = &region::LANGUAGES[usize::try_from(i).unwrap_or(0).min(region::LANGUAGES.len() - 1)];
            ui.set_language_index(i.max(0));
            ui.set_keyboard_index(region::keyboard_index(lang.keyboard) as i32);
            show_zone(lang.timezone);
        }
    };
    pick_language(0);
    ui.on_language_picked(pick_language);
    ui.on_tz_region_picked({
        let weak = ui.as_weak();
        let zones = zones.clone();
        move |r| {
            let ui = weak.unwrap();
            if let Some((_, cities)) = zones.get(usize::try_from(r).unwrap_or(0)) {
                ui.set_tz_cities(strings(cities.clone()));
                ui.set_tz_city_index(0);
            }
        }
    });

    // Network: status, Wi-Fi scan and connect, all off the UI thread.
    let networks: Arc<Mutex<Vec<network::Wifi>>> = Arc::new(Mutex::new(Vec::new()));
    let refresh = {
        let weak = ui.as_weak();
        let networks = networks.clone();
        move |scan: bool| {
            let weak = weak.clone();
            let networks = networks.clone();
            std::thread::spawn(move || {
                let online = network::online();
                let device = network::wifi_device();
                let list = match (&device, online, scan) {
                    (Some(dev), false, true) => network::scan(dev),
                    _ => Vec::new(),
                };
                let status = if online {
                    if network::cable() { "Connected with a network cable." } else { "Connected." }
                } else if device.is_some() {
                    "Not connected. Pick your Wi-Fi network, or plug in a network cable."
                } else {
                    "Not connected. Plug in a network cable; this computer has no Wi-Fi."
                };
                let rows: Vec<WifiRow> = list
                    .iter()
                    .take(5)
                    .map(|w| WifiRow {
                        name: w.name.clone().into(),
                        detail: format!(
                            "{} · signal {}",
                            if w.secured { "Secured" } else { "Open" },
                            "▮".repeat(w.signal.max(1))
                        )
                        .into(),
                    })
                    .collect();
                let had_scan = scan;
                *networks.lock().unwrap() = list;
                let _ = slint::invoke_from_event_loop(move || {
                    let ui = weak.unwrap();
                    ui.set_online(online);
                    ui.set_net_status(status.into());
                    ui.set_has_wifi(device.is_some());
                    if had_scan || online {
                        ui.set_wifi(ModelRc::new(VecModel::from(rows)));
                        ui.set_wifi_index(-1);
                    }
                });
            });
        }
    };
    ui.on_net_refresh({
        let refresh = refresh.clone();
        move || refresh(true)
    });
    ui.on_wifi_scan({
        let refresh = refresh.clone();
        move || refresh(true)
    });
    ui.on_wifi_connect({
        let weak = ui.as_weak();
        let networks = networks.clone();
        let refresh = refresh.clone();
        move || {
            let ui = weak.unwrap();
            let Some(name) = usize::try_from(ui.get_wifi_index())
                .ok()
                .and_then(|i| networks.lock().unwrap().get(i).map(|w| w.name.clone()))
            else {
                return;
            };
            let Some(device) = network::wifi_device() else { return };
            let password = ui.get_wifi_password().to_string();
            ui.set_wifi_busy(true);
            ui.set_error_text("".into());
            let weak = ui.as_weak();
            let refresh = refresh.clone();
            std::thread::spawn(move || {
                let result = network::connect(&device, &name, &password);
                let _ = slint::invoke_from_event_loop(move || {
                    let ui = weak.unwrap();
                    ui.set_wifi_busy(false);
                    match result {
                        Ok(()) => refresh(false),
                        Err(e) => ui.set_error_text(e.into()),
                    }
                });
            });
        }
    });

    // On-screen keyboard: append to or delete from the targeted field.
    ui.on_osk_typed({
        let weak = ui.as_weak();
        move |target, key| {
            let ui = weak.unwrap();
            let current = match target {
                0 => ui.get_username(),
                1 => ui.get_hostname(),
                2 => ui.get_password(),
                3 => ui.get_password2(),
                _ => ui.get_wifi_password(),
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
                3 => ui.set_password2(text),
                _ => ui.set_wifi_password(text),
            }
        }
    });

    ui.on_make_plan({
        let weak = ui.as_weak();
        let (inventory, disks, pending, zones) = (inventory.clone(), disks.clone(), pending.clone(), zones.clone());
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
                let pick = |i: i32, len: usize| usize::try_from(i).unwrap_or(0).min(len.saturating_sub(1));
                let language = &region::LANGUAGES[pick(ui.get_language_index(), region::LANGUAGES.len())];
                let keyboard = &region::KEYBOARDS[pick(ui.get_keyboard_index(), region::KEYBOARDS.len())];
                let (zone_region, cities) = &zones[pick(ui.get_tz_region_index(), zones.len())];
                let city = &cities[pick(ui.get_tz_city_index(), cities.len())];
                let config = InstallConfig {
                    disk: DiskConfig { device: disk.path.clone(), erase: true },
                    system: SystemConfig {
                        hostname: ui.get_hostname().to_string(),
                        timezone: format!("{zone_region}/{city}"),
                        locale: language.locale.into(),
                        keymap: keyboard.console.into(),
                        xkb_layout: keyboard.xkb.into(),
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
                    ui.set_page(4);
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
            ui.set_page(5);
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
                            ui.set_page(6);
                        }
                        Err(e) => {
                            ui.set_error_text(format!("{e:#}").into());
                            ui.set_page(7);
                        }
                    }
                });
            });
        }
    });

    ui.on_reboot(|| {
        let _ = Command::new("systemctl").arg("reboot").status();
    });

    if let Ok(device) = std::env::var("RENNER_TEST_CONTROL") {
        crate::control::start(device, ui.as_weak());
    }
    ui.run().map_err(|e| anyhow!("installer window failed: {e}"))
}
