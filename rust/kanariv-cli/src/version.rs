//! Build provenance, mirroring the C++ `version` subcommand.

use std::io::Write as _;

pub const FULL: &str = env!("KANARIV_VERSION_FULL");
pub const SHORT: &str = env!("KANARIV_SHORT_VERSION");
pub const COMMIT: &str = env!("KANARIV_GIT_HASH");
pub const COMPILER: &str = env!("KANARIV_RUSTC");

#[must_use]
pub fn std_label() -> &'static str {
    "2021"
}

#[must_use]
pub fn runtime_label() -> &'static str {
    "rustc-std"
}

#[must_use]
pub fn build_label() -> &'static str {
    if cfg!(debug_assertions) {
        "debug"
    } else {
        "release"
    }
}

#[must_use]
pub fn os_label() -> &'static str {
    if cfg!(target_os = "windows") {
        "windows"
    } else if cfg!(target_os = "macos") {
        "macos"
    } else if cfg!(target_os = "linux") {
        "linux"
    } else {
        "unknown"
    }
}

#[must_use]
pub fn arch_label() -> &'static str {
    if cfg!(target_arch = "x86_64") {
        "x86_64"
    } else if cfg!(target_arch = "aarch64") {
        "arm64"
    } else if cfg!(target_arch = "x86") {
        "x86"
    } else {
        "unknown"
    }
}

pub fn render(as_json: bool) -> i32 {
    if as_json {
        let value = serde_json::json!({
            "version": FULL,
            "short": SHORT,
            "commit": COMMIT,
            "compiler": COMPILER,
            "std": std_label(),
            "runtime": runtime_label(),
            "build": build_label(),
            "os": os_label(),
            "arch": arch_label(),
        });
        match serde_json::to_string(&value) {
            Ok(text) => println!("{text}"),
            Err(err) => {
                eprintln!("Error: Corrupt {err}");
                return 1;
            }
        }
        return 0;
    }

    let mut out = std::io::stdout().lock();
    let _ = writeln!(out, "kanariv {FULL}");
    let _ = writeln!(out, "  short:    {SHORT}");
    let _ = writeln!(out, "  commit:   {COMMIT}");
    let _ = writeln!(out, "  compiler: {COMPILER}");
    let _ = writeln!(out, "  std:      {}", std_label());
    let _ = writeln!(out, "  runtime:  {}", runtime_label());
    let _ = writeln!(out, "  build:    {}", build_label());
    let _ = writeln!(out, "  os:       {}", os_label());
    let _ = writeln!(out, "  arch:     {}", arch_label());
    0
}
