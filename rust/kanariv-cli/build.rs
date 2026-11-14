//! Embeds build provenance so `kanariv-rs version` mirrors the C++ CLI.
//!
//! Values come from the same sources as cmake/Version.cmake: the nearest git
//! tag (`vX.Y.Z`) and the short commit hash, with the VERSION file as the
//! fallback when git metadata is unavailable (source tarballs).

use std::fs;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let manifest_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").expect("manifest dir"));
    let repo_root = manifest_dir.join("..").join("..");

    let short = fs::read_to_string(repo_root.join("VERSION")).map_or_else(
        |_| std::env::var("CARGO_PKG_VERSION").expect("cargo version"),
        |text| text.trim().to_string(),
    );

    let full = git(&repo_root, &["describe", "--tags", "--abbrev=0"])
        .unwrap_or_else(|| format!("v{short}"));
    let commit =
        git(&repo_root, &["rev-parse", "--short", "HEAD"]).unwrap_or_else(|| "unknown".to_string());
    let rustc = std::env::var("RUSTC")
        .ok()
        .and_then(|rustc| {
            Command::new(rustc)
                .arg("--version")
                .output()
                .ok()
                .map(|out| String::from_utf8_lossy(&out.stdout).trim().to_string())
        })
        .unwrap_or_else(|| "rustc".to_string());

    println!("cargo:rustc-env=KANARIV_VERSION_FULL={full}");
    println!("cargo:rustc-env=KANARIV_SHORT_VERSION={short}");
    println!("cargo:rustc-env=KANARIV_GIT_HASH={commit}");
    println!("cargo:rustc-env=KANARIV_RUSTC={rustc}");
    println!("cargo:rerun-if-changed=../../VERSION");
}

fn git(dir: &PathBuf, args: &[&str]) -> Option<String> {
    let out = Command::new("git")
        .current_dir(dir)
        .args(args)
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&out.stdout).trim().to_string();
    (!text.is_empty()).then_some(text)
}
