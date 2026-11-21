//! Config discovery compatible with `kanariv::config`.
//!
//! Same precedence as the C++ library: `--config` override, else the
//! platform default (with the legacy Windows path as a fallback), tolerant
//! JSON reads, and derived socket/pid names.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

use serde_json::Value;

use crate::error::{Error, Result, StatusCode};
use crate::hash::endpoint_id_for_db;

#[derive(Clone, Debug)]
pub struct Config {
    pub db_path: PathBuf,
    pub config_path: PathBuf,
    pub device: String,
    pub device_name: String,
    pub log_level: i32,
    pub socket_path: String,
    pub auto_resume_on_play: bool,
    pub clear_stops_playback: bool,
    pub repeat_default: String,
    pub autostart: bool,
    pub replay_gain: String,
    pub fade_in_ms: i32,
    pub fade_out_ms: i32,
    pub inter_track_gap_ms: i32,
    pub crossfade_ms: i32,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            db_path: default_db_path(),
            config_path: default_config_path(),
            device: "auto".to_string(),
            device_name: String::new(),
            log_level: 2,
            socket_path: String::new(),
            auto_resume_on_play: false,
            clear_stops_playback: true,
            repeat_default: "off".to_string(),
            autostart: false,
            replay_gain: "off".to_string(),
            fade_in_ms: 0,
            fade_out_ms: 0,
            inter_track_gap_ms: 0,
            crossfade_ms: 0,
        }
    }
}

pub fn default_db_path() -> PathBuf {
    #[cfg(windows)]
    if let Some(local_app) = env::var_os("LOCALAPPDATA").filter(|v| !v.is_empty()) {
        return PathBuf::from(local_app).join("kanariv").join("library.db");
    }
    data_or_home_dir("XDG_DATA_HOME", &[".local", "share"]).join("library.db")
}

pub fn default_config_path() -> PathBuf {
    #[cfg(windows)]
    if let Some(local_app) = env::var_os("LOCALAPPDATA").filter(|v| !v.is_empty()) {
        return PathBuf::from(local_app).join("kanariv").join("config.json");
    }
    data_or_home_dir("XDG_CONFIG_HOME", &[".config"]).join("config.json")
}

fn legacy_config_path() -> Option<PathBuf> {
    #[cfg(windows)]
    {
        env::var_os("USERPROFILE")
            .filter(|v| !v.is_empty())
            .map(|home| {
                PathBuf::from(home)
                    .join(".config")
                    .join("kanariv")
                    .join("config.json")
            })
    }
    #[cfg(not(windows))]
    {
        None
    }
}

fn data_or_home_dir(xdg_var: &str, home_suffix: &[&str]) -> PathBuf {
    if let Some(xdg) = env::var_os(xdg_var).filter(|v| !v.is_empty()) {
        return PathBuf::from(xdg).join("kanariv");
    }
    let home = env::var_os("HOME")
        .filter(|v| !v.is_empty())
        .or_else(|| env::var_os("USERPROFILE").filter(|v| !v.is_empty()));
    home.map_or_else(
        || env::temp_dir().join("kanariv"),
        |home| {
            let mut base = PathBuf::from(home);
            for part in home_suffix {
                base.push(part);
            }
            base.join("kanariv")
        },
    )
}

fn base_dir_for_socket() -> PathBuf {
    #[cfg(windows)]
    if let Some(local_app) = env::var_os("LOCALAPPDATA").filter(|v| !v.is_empty()) {
        return PathBuf::from(local_app).join("kanariv");
    }
    if let Some(runtime) = env::var_os("XDG_RUNTIME_DIR").filter(|v| !v.is_empty()) {
        return PathBuf::from(runtime).join("kanariv");
    }
    data_or_home_dir("XDG_DATA_HOME", &[".local", "share"])
}

/// `std::filesystem::path::generic_string` equivalent for hash inputs.
#[must_use]
pub fn generic_string(path: &Path) -> String {
    let rendered = path.to_string_lossy().into_owned();
    if cfg!(windows) {
        rendered.replace('\\', "/")
    } else {
        rendered
    }
}

#[must_use]
pub fn socket_path_for(db_path: &Path) -> String {
    let id = endpoint_id_for_db(&generic_string(db_path));
    #[cfg(windows)]
    {
        format!(r"\\.\pipe\kanariv-{id}")
    }
    #[cfg(not(windows))]
    {
        generic_string(&base_dir_for_socket().join(format!("kanariv-{id}.sock")))
    }
}

#[must_use]
pub fn pid_path_for(db_path: &Path) -> PathBuf {
    let id = endpoint_id_for_db(&generic_string(db_path));
    base_dir_for_socket().join(format!("kanariv-{id}.pid"))
}

/// Loads config, returning platform defaults when the file is absent.
///
/// A present-but-invalid file surfaces `Corrupt`; the caller keeps defaults
/// (the C++ shell behaves the same way).
pub fn load_config(override_path: Option<&Path>) -> Result<Config> {
    let mut cfg = Config::default();
    if let Some(path) = override_path {
        cfg.config_path = path.to_path_buf();
    }

    let mut file = cfg.config_path.clone();
    if override_path.is_none() && !file.exists() {
        if let Some(legacy) = legacy_config_path().filter(|p| p.exists()) {
            cfg.config_path.clone_from(&legacy);
            file = legacy;
        }
    }

    if !file.exists() {
        return Ok(cfg);
    }

    let content =
        fs::read_to_string(&file).map_err(|e| Error::io(format!("cannot open config: {e}")))?;
    if content.is_empty() {
        return Ok(cfg);
    }

    let json: Value = serde_json::from_str(&content)
        .map_err(|e| Error::new(StatusCode::Corrupt, e.to_string()))?;
    apply_json(&mut cfg, &json);
    Ok(cfg)
}

fn apply_json(cfg: &mut Config, json: &Value) {
    if let Some(value) = json
        .get("dbPath")
        .and_then(Value::as_str)
        .filter(|s| !s.is_empty())
    {
        cfg.db_path = PathBuf::from(value);
    }
    if let Some(value) = json.get("device").and_then(Value::as_str) {
        cfg.device = value.to_string();
    }
    if let Some(value) = json.get("deviceName").and_then(Value::as_str) {
        cfg.device_name = value.to_string();
    }
    if let Some(value) = json.get("logLevel") {
        if let Some(level) = parse_log_level(value) {
            cfg.log_level = level;
        }
    }
    if let Some(value) = json
        .get("socketPath")
        .and_then(Value::as_str)
        .filter(|s| !s.is_empty())
    {
        cfg.socket_path = value.to_string();
    }
    if let Some(value) = json.get("autoResumeOnPlay").and_then(Value::as_bool) {
        cfg.auto_resume_on_play = value;
    }
    if let Some(value) = json.get("clearStopsPlayback").and_then(Value::as_bool) {
        cfg.clear_stops_playback = value;
    }
    if let Some(value) = json.get("repeatDefault").and_then(Value::as_str) {
        cfg.repeat_default = match value.to_ascii_lowercase().as_str() {
            "one" => "one".to_string(),
            "all" => "all".to_string(),
            _ => "off".to_string(),
        };
    }
    if let Some(value) = json.get("autostart").and_then(Value::as_bool) {
        cfg.autostart = value;
    }
    if let Some(value) = json.get("replayGain").and_then(Value::as_str) {
        cfg.replay_gain = match value.to_ascii_lowercase().as_str() {
            "track" => "track".to_string(),
            "album" => "album".to_string(),
            _ => "off".to_string(),
        };
    }
    for (key, target) in [
        ("fadeInMs", &mut cfg.fade_in_ms),
        ("fadeOutMs", &mut cfg.fade_out_ms),
        ("interTrackGapMs", &mut cfg.inter_track_gap_ms),
    ] {
        if let Some(value) = json.get(key).and_then(Value::as_i64) {
            *target = i32::try_from(value).unwrap_or(*target);
        }
    }
    if let Some(value) = json.get("crossfadeMs").and_then(Value::as_i64) {
        cfg.crossfade_ms = i32::try_from(value).unwrap_or(cfg.crossfade_ms);
    } else if cfg.inter_track_gap_ms > 0 {
        // Legacy meaning: before the split, interTrackGapMs configured the
        // crossfade overlap; a config without crossfadeMs keeps it.
        cfg.crossfade_ms = cfg.inter_track_gap_ms;
        cfg.inter_track_gap_ms = 0;
    }

    // Legacy spellings (db_path / log_level) win when both are present,
    // matching the C++ reader's assignment order.
    if let Some(value) = json
        .get("db_path")
        .and_then(Value::as_str)
        .filter(|s| !s.is_empty())
    {
        cfg.db_path = PathBuf::from(value);
    }
    if let Some(level) = json.get("log_level").and_then(parse_log_level) {
        cfg.log_level = level;
    }
}

/// Reads one raw key from the config file, matching
/// `config::detail::configGetRaw`: the daemon is down, so the file is the
/// authority. JSON strings come back bare, `null` as `"null"`, anything else
/// as compact JSON.
pub fn get_raw(path: &Path, key: &str) -> Result<String> {
    if !path.exists() {
        return Err(Error::new(StatusCode::NotFound, "config not found"));
    }
    let content =
        fs::read_to_string(path).map_err(|e| Error::io(format!("cannot open config: {e}")))?;
    if content.is_empty() {
        return Err(Error::new(
            StatusCode::NotFound,
            format!("key not found: {key}"),
        ));
    }
    let json: Value = serde_json::from_str(&content)
        .map_err(|e| Error::new(StatusCode::Corrupt, e.to_string()))?;
    if !json.is_object() {
        return Err(Error::new(StatusCode::Corrupt, "config is not an object"));
    }
    match json.get(key) {
        Some(Value::String(text)) => Ok(text.clone()),
        Some(Value::Null) => Ok("null".to_string()),
        Some(value) => {
            serde_json::to_string(value).map_err(|e| Error::new(StatusCode::Corrupt, e.to_string()))
        }
        None => Err(Error::new(
            StatusCode::NotFound,
            format!("key not found: {key}"),
        )),
    }
}

#[must_use]
pub fn is_valid_log_level(name: &str) -> bool {
    parse_log_level(&Value::String(name.to_string())).is_some()
}

fn parse_log_level(value: &Value) -> Option<i32> {
    if let Some(number) = value.as_i64() {
        // Out-of-range numbers keep their direction, like `clampLogLevel`.
        return i32::try_from(number.clamp(0, 4)).ok();
    }
    let name = value.as_str()?;
    if name.len() == 1 {
        if let Some(digit) = name.chars().next().filter(char::is_ascii_digit) {
            return digit.to_digit(10).and_then(|d| i32::try_from(d).ok());
        }
    }
    match name {
        "trace" => Some(0),
        "debug" => Some(1),
        "info" => Some(2),
        "warn" => Some(3),
        "error" => Some(4),
        _ => None,
    }
}

pub fn save_config(cfg: &Config) -> Result<()> {
    if let Some(parent) = cfg.config_path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent).map_err(|e| {
                Error::io(format!(
                    "cannot create config directory '{}': {e}",
                    generic_string(parent)
                ))
            })?;
        }
    }
    // Key order matches `config::saveConfig`; the daemon's `config list`
    // reports file order, so preserving it keeps the output identical.
    let mut json = serde_json::json!({
        "dbPath": generic_string(&cfg.db_path),
        "device": cfg.device,
        "deviceName": cfg.device_name,
        "logLevel": cfg.log_level,
        "autoResumeOnPlay": cfg.auto_resume_on_play,
        "clearStopsPlayback": cfg.clear_stops_playback,
        "repeatDefault": cfg.repeat_default,
        "autostart": cfg.autostart,
        "replayGain": cfg.replay_gain,
        "fadeInMs": cfg.fade_in_ms,
        "fadeOutMs": cfg.fade_out_ms,
        "interTrackGapMs": cfg.inter_track_gap_ms,
        "crossfadeMs": cfg.crossfade_ms,
    });
    if !cfg.socket_path.is_empty() {
        json["socketPath"] = Value::String(cfg.socket_path.clone());
    }
    let text = serde_json::to_string_pretty(&json)
        .map_err(|e| Error::new(StatusCode::Corrupt, e.to_string()))?;

    // Write beside the target first so a failure never truncates the live
    // config, then rename; the copy fallback covers filesystems that reject
    // rename-over-existing (Windows). Same sequence as `config::saveConfig`.
    let mut tmp_name = cfg.config_path.clone().into_os_string();
    tmp_name.push(".tmp");
    let tmp = PathBuf::from(tmp_name);
    if let Err(err) = fs::write(&tmp, &text) {
        let _ = fs::remove_file(&tmp);
        return Err(Error::io(format!(
            "cannot write config '{}': {err}",
            generic_string(&tmp)
        )));
    }
    if let Err(rename_err) = fs::rename(&tmp, &cfg.config_path) {
        let copied = fs::copy(&tmp, &cfg.config_path).is_ok();
        let _ = fs::remove_file(&tmp);
        if !copied {
            return Err(Error::io(format!(
                "cannot replace config '{}': {rename_err}",
                generic_string(&cfg.config_path)
            )));
        }
    }
    Ok(())
}

/// Applies `--config`, `--log-level`, `--device` and derives the socket.
///
/// Mirrors the shell's post-parse sequence. First-run defaults are *not*
/// written here: the caller persists them only for mutating commands,
/// matching C++ `PersistDefaultsGuard` (`status`/`config get` must not
/// create files).
pub fn resolve(
    config_override: Option<&Path>,
    log_level: Option<&str>,
    device: Option<&str>,
) -> Result<Config> {
    let mut cfg = if let Ok(cfg) = load_config(config_override) {
        cfg
    } else {
        // Corrupt config: the C++ shell silently keeps defaults, so a broken
        // file must not block status/start commands.
        let mut defaults = Config::default();
        if let Some(path) = config_override {
            defaults.config_path = path.to_path_buf();
        }
        defaults
    };
    if let Some(path) = config_override {
        cfg.config_path = path.to_path_buf();
    }
    if let Some(device) = device {
        cfg.device = device.to_string();
    }
    if let Some(level) = log_level {
        cfg.log_level = parse_log_level(&Value::String(level.to_string())).ok_or_else(|| {
            Error::invalid_arg(format!(
                "--log-level: invalid '{level}' (expected trace|debug|info|warn|error)"
            ))
        })?;
    }
    if cfg.socket_path.is_empty() && !cfg.db_path.as_os_str().is_empty() {
        cfg.socket_path = socket_path_for(&cfg.db_path);
    }
    if let Some(parent) = cfg.db_path.parent() {
        let _ = fs::create_dir_all(parent);
    }
    Ok(cfg)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn generic_string_uses_forward_slashes() {
        let path = PathBuf::from("a").join("b").join("library.db");
        assert_eq!(generic_string(&path), "a/b/library.db");
    }

    #[test]
    fn log_level_accepts_names_and_digits() {
        assert_eq!(parse_log_level(&Value::String("trace".into())), Some(0));
        assert_eq!(parse_log_level(&Value::String("warn".into())), Some(3));
        assert_eq!(parse_log_level(&Value::String("2".into())), Some(2));
        assert_eq!(parse_log_level(&Value::from(4)), Some(4));
        assert_eq!(parse_log_level(&Value::String("loud".into())), None);
    }
}
