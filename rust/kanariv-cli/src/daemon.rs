//! Daemon lifecycle: spawn, readiness, shutdown.
//!
//! The Rust CLI is frontend-only in this phase, so spawning targets the
//! existing C++ daemon binary: `KANARIV_DAEMON_BIN` when set, otherwise a
//! sibling `kanariv` executable. Readiness is a full `StatusReq` round-trip,
//! not just a connect, so a draining daemon is never reported as started.

use std::env;
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::thread;
use std::time::Duration;

use crate::config::{self, Config};
use crate::error::{Error, Result, StatusCode};
use crate::outcome::Outcome;
use crate::{client, ipc};

pub fn start(config: &Config, foreground: bool) -> Result<Outcome> {
    if client::probe(&config.socket_path) {
        return Ok(Outcome::ok(
            None,
            Some(format!("daemon already running at {}", config.socket_path)),
        ));
    }

    wait_for_teardown(config);

    if let Some(parent) = config.db_path.parent() {
        let _ = fs::create_dir_all(parent);
    }

    if foreground {
        return run_foreground(config);
    }

    let binary = daemon_binary()?;
    let mut command = Command::new(&binary);
    command.args(["--daemon", "--foreground"]);
    if !config.config_path.as_os_str().is_empty() {
        command.arg("--config").arg(&config.config_path);
    }
    command
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    detach(&mut command);
    if let Err(err) = command.spawn() {
        return Ok(Outcome::fail(format!(
            "start failed: couldn't start daemon ({err})"
        )));
    }

    let mut last_detail = String::new();
    for _ in 0..50 {
        match client::send(
            &config.socket_path,
            &ipc::Command::StatusReq,
            Duration::from_millis(500),
        ) {
            Ok(payload) if payload.status().is_some() => {
                return Ok(Outcome::ok(
                    None,
                    Some(format!("daemon started at {}", config.socket_path)),
                ));
            }
            Ok(_) => last_detail = "unexpected reply".to_string(),
            Err(err) => last_detail = err.message,
        }
        thread::sleep(Duration::from_millis(100));
    }

    let mut message = format!(
        "daemon start failed: socket not reachable at {}",
        config.socket_path
    );
    if !last_detail.is_empty() {
        let _ = write!(message, "\nlast error: {last_detail}");
    }
    Ok(Outcome::fail(message))
}

pub fn shutdown(config: &Config) -> Outcome {
    let result = client::send(
        &config.socket_path,
        &ipc::Command::Shutdown,
        Duration::from_millis(2000),
    );
    if let Err(err) = result {
        if err.code == StatusCode::State {
            return Outcome::fail("shutdown: daemon not running");
        }
    }

    let pid_path = config::pid_path_for(&config.db_path);
    for _ in 0..20 {
        thread::sleep(Duration::from_millis(100));
        let pid_exists = pid_path.exists();
        if !client::probe(&config.socket_path) && !pid_exists {
            break;
        }
    }

    if client::probe(&config.socket_path) {
        return Outcome::fail(format!(
            "shutdown: daemon still running at {}",
            config.socket_path
        ));
    }
    Outcome::ok(None, Some("daemon stopped".to_string()))
}

fn run_foreground(config: &Config) -> Result<Outcome> {
    let binary = daemon_binary()?;
    let status = Command::new(&binary)
        .args(["--config"])
        .arg(&config.config_path)
        .args(["--daemon", "--foreground"])
        .status()
        .map_err(|err| Error::io(format!("start failed: couldn't start daemon ({err})")))?;
    if !status.success() {
        return Ok(Outcome::fail(format!(
            "daemon error: exited with {}",
            status.code().unwrap_or(1)
        )));
    }
    Ok(Outcome::ok(
        None,
        Some(format!(
            "starting daemon foreground at {}",
            config.socket_path
        )),
    ))
}

fn daemon_binary() -> Result<PathBuf> {
    if let Some(path) = env::var_os("KANARIV_DAEMON_BIN").filter(|v| !v.is_empty()) {
        return Ok(PathBuf::from(path));
    }
    let name = if cfg!(windows) {
        "kanariv.exe"
    } else {
        "kanariv"
    };
    let sibling = env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(|dir| dir.join(name)));
    sibling.filter(|path| path.exists()).ok_or_else(|| {
        Error::new(
            StatusCode::Unsupported,
            "daemon binary not found; set KANARIV_DAEMON_BIN to the kanariv executable",
        )
    })
}

fn wait_for_teardown(config: &Config) {
    let pid_path = config::pid_path_for(&config.db_path);
    for _ in 0..20 {
        let connected = client::probe(&config.socket_path);
        let pid_exists = pid_path.exists();
        let alive = pid_exists && pid_file_alive(&pid_path);
        if !connected && !alive {
            if pid_exists {
                let _ = fs::remove_file(&pid_path);
            }
            return;
        }
        thread::sleep(Duration::from_millis(100));
    }
}

/// Conservative liveness check: unreadable or garbage files count as live so
/// a running daemon is never declared dead. Linux uses /proc; other hosts
/// keep the full wait rather than risk a wrong fast path.
fn pid_file_alive(path: &Path) -> bool {
    let Ok(text) = fs::read_to_string(path) else {
        return true;
    };
    let Ok(pid) = text.trim().parse::<i64>() else {
        return true;
    };
    if pid <= 0 {
        return true;
    }
    #[cfg(target_os = "linux")]
    {
        Path::new(&format!("/proc/{pid}")).exists()
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = pid;
        true
    }
}

#[cfg(unix)]
fn detach(command: &mut Command) {
    use std::os::unix::process::CommandExt;
    command.process_group(0);
}

#[cfg(windows)]
fn detach(command: &mut Command) {
    use std::os::windows::process::CommandExt;
    const DETACHED_PROCESS: u32 = 0x0000_0008;
    command.creation_flags(DETACHED_PROCESS);
}

#[cfg(not(any(unix, windows)))]
fn detach(_command: &mut Command) {}
