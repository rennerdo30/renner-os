//! renner-installer: installs renner.dev OS.
//!
//!   renner-installer                         graphical installer (GUI builds)
//!   renner-installer --config install.toml   unattended install, no questions
//!   renner-installer --config x.toml --check print the plan, change nothing

use anyhow::{Context, Result};
use clap::Parser;
use installer_core::engine::{self, Event, STAGES};
use installer_core::{InstallConfig, Inventory, plan};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

#[cfg(feature = "gui")]
mod console;
#[cfg(feature = "gui")]
mod gui;

#[derive(Parser)]
#[command(name = "renner-installer", version, about = "Install renner.dev OS")]
struct Args {
    /// Unattended install from this TOML config (see docs/unattended.md).
    #[arg(long, value_name = "FILE")]
    config: Option<PathBuf>,
    /// Only validate the config against this machine and print the plan.
    #[arg(long, requires = "config")]
    check: bool,
    /// Print every installer output line, not only the stages.
    #[arg(long)]
    verbose: bool,
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("renner-installer: {e:#}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<()> {
    let args = Args::parse();
    match &args.config {
        Some(path) => unattended(path, args.check, args.verbose),
        None => graphical(),
    }
}

/// Resolves `/dev/disk/by-id/...` and other aliases to the kernel device.
pub fn resolve_device(device: &str) -> String {
    std::fs::canonicalize(device)
        .map(|p| p.to_string_lossy().into_owned())
        .unwrap_or_else(|_| device.to_string())
}

fn unattended(path: &Path, check: bool, verbose: bool) -> Result<()> {
    let text = std::fs::read_to_string(path).with_context(|| format!("cannot read {}", path.display()))?;
    let config = InstallConfig::from_toml(&text).with_context(|| format!("in {}", path.display()))?;
    let inventory = Inventory::detect()?;
    let plan = plan::build(&config, &inventory, &resolve_device(&config.disk.device))?;
    println!("{plan}");
    if check {
        println!("\n--check: nothing was changed.");
        return Ok(());
    }
    println!("\nUnattended install started.");
    engine::run(&plan, |event| match event {
        Event::Stage(i, text) => println!("[{}/{}] {text}", i + 1, STAGES.len()),
        Event::Line(line) if verbose => println!("    {line}"),
        Event::Line(_) => {}
    })?;
    println!("renner.dev OS is installed. Remove the install medium and restart.");
    Ok(())
}

#[cfg(feature = "gui")]
fn graphical() -> Result<()> {
    gui::run()
}

#[cfg(not(feature = "gui"))]
fn graphical() -> Result<()> {
    anyhow::bail!("this build has no graphical installer; use --config FILE for an unattended install")
}
