//! Keeps the text console out of the way while the graphical installer runs.
//!
//! The KMS backend reads the keyboard through libinput, but the virtual
//! terminal underneath still receives every key: typed text (passwords
//! included) would pile up in the shell, and Ctrl+C or Ctrl+\ would kill the
//! installer in the middle of an install (RO-30). Like X and Wayland
//! sessions, the installer therefore puts the console into graphics mode and
//! switches its keyboard off while it runs, and restores both on exit.

#[cfg(target_os = "linux")]
mod imp {
    use std::fs::{File, OpenOptions};
    use std::os::fd::AsRawFd;

    const KDSETMODE: libc::c_ulong = 0x4B3A;
    const KD_TEXT: libc::c_ulong = 0;
    const KD_GRAPHICS: libc::c_ulong = 1;
    const KDGKBMODE: libc::c_ulong = 0x4B44;
    const KDSKBMODE: libc::c_ulong = 0x4B45;
    const K_OFF: libc::c_ulong = 4;

    pub struct ConsoleGuard {
        tty: File,
        keyboard_mode: libc::c_int,
    }

    impl ConsoleGuard {
        /// Silences the controlling console. `None` when there is none (for
        /// example over SSH), which is fine: then no keys leak either.
        pub fn engage() -> Option<Self> {
            let tty = OpenOptions::new().read(true).write(true).open("/dev/tty").ok()?;
            let fd = tty.as_raw_fd();
            let mut keyboard_mode: libc::c_int = 0;
            // SAFETY: plain ioctls on our own console descriptor.
            unsafe {
                if libc::ioctl(fd, KDGKBMODE, &mut keyboard_mode) != 0 {
                    return None; // not a virtual console
                }
                libc::ioctl(fd, KDSKBMODE, K_OFF);
                libc::ioctl(fd, KDSETMODE, KD_GRAPHICS);
            }
            Some(Self { tty, keyboard_mode })
        }
    }

    impl Drop for ConsoleGuard {
        fn drop(&mut self) {
            let fd = self.tty.as_raw_fd();
            // SAFETY: restoring the state saved in engage() on the same fd;
            // TCIFLUSH drops anything that reached the console meanwhile.
            unsafe {
                libc::ioctl(fd, KDSETMODE, KD_TEXT);
                libc::ioctl(fd, KDSKBMODE, self.keyboard_mode as libc::c_ulong);
                libc::tcflush(fd, libc::TCIFLUSH);
            }
        }
    }

    /// Terminal signals never stop the installer (and, inherited, the install).
    pub fn ignore_terminal_signals() {
        // SAFETY: setting signal dispositions to SIG_IGN at startup.
        unsafe {
            libc::signal(libc::SIGINT, libc::SIG_IGN);
            libc::signal(libc::SIGQUIT, libc::SIG_IGN);
            libc::signal(libc::SIGHUP, libc::SIG_IGN);
            libc::signal(libc::SIGTSTP, libc::SIG_IGN);
        }
    }
}

#[cfg(not(target_os = "linux"))]
mod imp {
    pub struct ConsoleGuard;
    impl ConsoleGuard {
        pub fn engage() -> Option<Self> {
            None
        }
    }
    pub fn ignore_terminal_signals() {}
}

pub use imp::{ConsoleGuard, ignore_terminal_signals};
