//! renner.dev OS installer engine, shared by the GUI, the TUI and unattended
//! installs: config → detection → plan → run. No UI code lives here.

pub mod config;
pub mod detect;
pub mod engine;
pub mod plan;

pub use config::InstallConfig;
pub use detect::Inventory;
pub use plan::Plan;
