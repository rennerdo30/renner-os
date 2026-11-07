//! Lab control channel: drives the real installer UI over a serial port, so
//! tests do not depend on clicks landing on the right pixels. Only active when
//! RENNER_TEST_CONTROL names a device (the lab ISO's autostart sets it for
//! `renner.test=1`); one command per line, one reply line per command.
//!
//!   state                              page=3 online=1 progress=0.00 error=""
//!   set username|hostname|password|password2|wifi-password VALUE
//!   pick language|keyboard|tz-region|tz-city|disk|wifi INDEX
//!   next | back                        what the Next / Back buttons do

use crate::gui::InstallerWindow;

use std::io::{BufRead, BufReader, Write};
use std::sync::mpsc;
use std::time::Duration;

pub fn start(device: String, weak: slint::Weak<InstallerWindow>) {
    std::thread::spawn(move || {
        let Ok(port) = std::fs::OpenOptions::new().read(true).write(true).open(&device) else {
            eprintln!("renner-installer: cannot open control device {device}");
            return;
        };
        let Ok(reader) = port.try_clone() else { return };
        let mut writer = port;
        for line in BufReader::new(reader).lines() {
            let Ok(line) = line else { break };
            let line = line.trim().to_string();
            if line.is_empty() {
                continue;
            }
            let (tx, rx) = mpsc::channel();
            let weak = weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                let reply = match weak.upgrade() {
                    Some(ui) => handle(&ui, &line),
                    None => "err no window".into(),
                };
                let _ = tx.send(reply);
            });
            let reply = rx.recv_timeout(Duration::from_secs(10)).unwrap_or_else(|_| "err timeout".into());
            let _ = writeln!(writer, "{reply}");
            let _ = writer.flush();
        }
    });
}

fn handle(ui: &InstallerWindow, line: &str) -> String {
    let mut parts = line.splitn(3, ' ');
    let cmd = parts.next().unwrap_or("");
    let what = parts.next().unwrap_or("");
    let value = parts.next().unwrap_or("");
    match cmd {
        "state" => format!(
            "page={} online={} progress={:.2} error={:?}",
            ui.get_page(),
            u8::from(ui.get_online()),
            ui.get_progress(),
            ui.get_error_text().to_string()
        ),
        "next" => {
            ui.invoke_go_next();
            format!("ok page={}", ui.get_page())
        }
        "back" => {
            ui.invoke_go_back();
            format!("ok page={}", ui.get_page())
        }
        "set" => {
            let v = slint::SharedString::from(value);
            match what {
                "username" => ui.set_username(v),
                "hostname" => ui.set_hostname(v),
                "password" => ui.set_password(v),
                "password2" => ui.set_password2(v),
                "wifi-password" => ui.set_wifi_password(v),
                _ => return format!("err unknown field {what}"),
            }
            "ok".into()
        }
        "pick" => {
            let Ok(i) = value.parse::<i32>() else { return "err index".into() };
            match what {
                "language" => ui.invoke_language_picked(i),
                "keyboard" => ui.set_keyboard_index(i),
                "tz-region" => {
                    ui.set_tz_region_index(i);
                    ui.invoke_tz_region_picked(i);
                }
                "tz-city" => ui.set_tz_city_index(i),
                "disk" => ui.set_disk_index(i),
                "wifi" => ui.set_wifi_index(i),
                _ => return format!("err unknown list {what}"),
            }
            "ok".into()
        }
        _ => format!("err unknown command {cmd}"),
    }
}
