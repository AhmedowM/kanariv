//! Playback/queue handlers, mirroring `kanariv::app`.
//!
//! Rendering stays out of here: handlers return payload + optional line,
//! exactly like the C++ `Outcome` contract.

use std::fmt::Write as _;
use std::thread;
use std::time::Duration;

use crate::batch::BatchReport;
use crate::client::{self, DEFAULT_TIMEOUT};
use crate::config::Config;
use crate::error::{Result, StatusCode};
use crate::ipc::{Command, ConfigValue, Payload, RepeatMode, Reply, Status, Track};
use crate::outcome::Outcome;

pub struct App {
    config: Config,
}

impl App {
    #[must_use]
    pub fn new(config: Config) -> Self {
        Self { config }
    }

    #[must_use]
    pub fn config(&self) -> &Config {
        &self.config
    }

    fn send(&self, command: &Command) -> Result<Payload> {
        client::send(&self.config.socket_path, command, DEFAULT_TIMEOUT)
    }

    fn send_play(&self, command: &Command) -> Result<Payload> {
        match self.send(command) {
            Err(err) if err.code == StatusCode::State && err.message == "daemon not running" => {
                // Autostart, then retry once. A failed start still retries so
                // the user sees the real daemon error ("empty queue", etc.).
                let _ = crate::daemon::start(&self.config, false);
                self.send(command)
            }
            other => other,
        }
    }

    fn confirm(&self, command: &Command, line: Option<String>) -> Result<Outcome> {
        Ok(Outcome::confirm(self.send(command)?, line))
    }

    pub fn status(&self) -> Result<Outcome> {
        self.confirm(&Command::StatusReq, None)
    }

    pub fn info(&self) -> Result<Outcome> {
        self.confirm(&Command::Info, None)
    }

    pub fn set_volume(
        &self,
        level: Option<f32>,
        mute: Option<bool>,
        delta_pct: Option<i32>,
    ) -> Result<Outcome> {
        Ok(Outcome::confirm(
            self.send(&Command::VolumeSet {
                level,
                mute,
                delta_pct,
            })?,
            None,
        ))
    }

    pub fn play(&self) -> Result<Outcome> {
        let (was_paused, was_stopped, prior_pos, prior_track) = match self.send(&Command::StatusReq)
        {
            Ok(payload) => match payload.status() {
                Some(status) => (
                    status.is_paused(),
                    status.is_stopped(),
                    status.pos,
                    status.track_id,
                ),
                None => (false, false, 0.0, 0),
            },
            Err(_) => (false, false, 0.0, 0),
        };

        let mut payload = self.send_play(&Command::Play)?;
        let Some(status) = payload.status() else {
            return Ok(Outcome::confirm(payload, None));
        };

        // Session restore: the daemon may report the persisted position
        // while stopped. If it did not land near the saved point, seek once
        // and refresh the status before building the line.
        let mut restored = false;
        if was_stopped && prior_track != 0 && prior_pos > 0.0 && status.track_id == prior_track {
            restored = (prior_pos - 0.5..=prior_pos + 0.5).contains(&status.pos);
            if !restored && self.send(&Command::Seek { seconds: prior_pos }).is_ok() {
                if let Ok(fresh) = self.send(&Command::StatusReq) {
                    payload = fresh;
                }
                restored = true;
            }
        }

        let who = payload
            .status()
            .map_or_else(|| "unknown track".to_string(), track_who);
        let line = if was_paused || restored {
            format!("Resuming {who} from {}", format_clock(prior_pos))
        } else {
            format!("Playing {who}")
        };
        Ok(Outcome::confirm(payload, Some(line)))
    }

    pub fn pause(&self) -> Result<Outcome> {
        match self.send(&Command::Pause) {
            Err(err) if err.code == StatusCode::State && err.message == "not playing" => {
                Ok(Outcome::warn("pause: nothing playing"))
            }
            Err(err) => Err(err),
            Ok(payload) => {
                let who = payload
                    .status()
                    .map_or_else(|| "unknown track".to_string(), track_who);
                let pos = payload.status().map_or(0.0, |status| status.pos);
                Ok(Outcome::confirm(
                    payload,
                    Some(format!("Paused {who} at {}", format_clock(pos))),
                ))
            }
        }
    }

    pub fn resume(&self) -> Result<Outcome> {
        match self.send(&Command::Resume) {
            Err(err) if err.code == StatusCode::State && err.message == "not paused" => {
                let probe = self.send(&Command::StatusReq);
                match probe {
                    Ok(payload) if payload.status().is_some_and(Status::is_playing) => {
                        Ok(Outcome::warn("resume: already playing"))
                    }
                    Ok(_) => self.play(),
                    Err(_) => Err(err),
                }
            }
            Err(err) => Err(err),
            Ok(payload) => {
                let who = payload
                    .status()
                    .map_or_else(|| "unknown track".to_string(), track_who);
                let at = payload.status().map_or(0.0, |status| status.pos);
                Ok(Outcome::confirm(
                    payload,
                    Some(format!("Resuming {who} from {}", format_clock(at))),
                ))
            }
        }
    }

    pub fn restart(&self) -> Result<Outcome> {
        let payload = self.send(&Command::Restart)?;
        let who = payload
            .status()
            .map_or_else(|| "unknown track".to_string(), track_who);
        Ok(Outcome::confirm(payload, Some(format!("Restarting {who}"))))
    }

    pub fn stop(&self) -> Result<Outcome> {
        self.confirm(&Command::Stop, Some("Stopped".to_string()))
    }

    pub fn next(&self) -> Result<Outcome> {
        let payload = match self.send(&Command::Next) {
            Err(err) if err.code == StatusCode::Busy => {
                thread::sleep(Duration::from_millis(50));
                self.send(&Command::Next)
            }
            other => other,
        };
        match payload {
            Err(err) if err.code == StatusCode::Busy => {
                Ok(Outcome::fail("next: engine busy, retry"))
            }
            Err(err) => Err(err),
            Ok(payload) => {
                let who = payload
                    .status()
                    .map_or_else(|| "unknown track".to_string(), track_who);
                Ok(Outcome::confirm(payload, Some(format!("Playing {who}"))))
            }
        }
    }

    pub fn prev(&self) -> Result<Outcome> {
        let payload = match self.send(&Command::Prev) {
            Err(err) if err.code == StatusCode::Busy => {
                thread::sleep(Duration::from_millis(50));
                self.send(&Command::Prev)
            }
            other => other,
        };
        match payload {
            Err(err) if err.code == StatusCode::Busy => {
                Ok(Outcome::fail("prev: engine busy, retry"))
            }
            Err(err) if err.code == StatusCode::NotFound && err.message == "at start" => {
                Ok(Outcome::warn("prev: at queue start"))
            }
            Err(err) => Err(err),
            Ok(payload) => {
                let who = payload
                    .status()
                    .map_or_else(|| "unknown track".to_string(), track_who);
                Ok(Outcome::confirm(payload, Some(format!("Playing {who}"))))
            }
        }
    }

    pub fn seek(&self, mut target: f64, relative: bool) -> Result<Outcome> {
        if relative {
            let position = self
                .send(&Command::StatusReq)
                .ok()
                .and_then(|payload| payload.status().map(|status| status.pos));
            target =
                position.map_or_else(|| target.max(0.0), |position| (position + target).max(0.0));
        }
        Ok(Outcome {
            payload: Some(self.send(&Command::Seek { seconds: target })?),
            line: None,
            to_stderr: false,
            failed: false,
            quiet: true,
        })
    }

    pub fn queue_list(&self) -> Result<Outcome> {
        self.confirm(&Command::QueueQueues, None)
    }

    pub fn queue_tracks(&self, order: &str) -> Result<(Payload, i64)> {
        let payload = self.send(&Command::QueueList {
            order: order.to_string(),
        })?;
        let highlight = self
            .send(&Command::StatusReq)
            .ok()
            .and_then(|status| status.status().map(|status| status.track_id))
            .unwrap_or(0);
        Ok((payload, highlight))
    }

    pub fn queue_switch(&self, qid: i64) -> Result<Outcome> {
        self.confirm(
            &Command::QueueSwitch { qid },
            Some(format!("Switched to queue {qid}")),
        )
    }

    pub fn queue_create(&self, name: &str) -> Result<Outcome> {
        let payload = self.send(&Command::QueueCreate {
            name: name.to_string(),
        })?;
        let line = match &payload.reply {
            Reply::QueueCreated { id, name } => Some(format!("Created queue {id} '{name}'")),
            _ => None,
        };
        Ok(Outcome::confirm(payload, line))
    }

    pub fn queue_delete(&self, qid: i64) -> Result<Outcome> {
        self.confirm(
            &Command::QueueDelete { qid },
            Some(format!("Deleted queue {qid}")),
        )
    }

    pub fn queue_clear(&self) -> Result<Outcome> {
        self.confirm(&Command::QueueClear, Some("Queue cleared".to_string()))
    }

    pub fn queue_shuffle(&self, mode: Option<&str>) -> Result<Outcome> {
        let on = match mode {
            None => None,
            Some("on") => Some(true),
            Some("off") => Some(false),
            Some(other) => {
                return Ok(Outcome::fail(format!(
                    "shuffle: invalid mode '{other}' (expected on|off)"
                )))
            }
        };
        let payload = self.send(&Command::QueueShuffle { on })?;
        let resulting = payload.status().map(|status| status.shuffle).or(on);
        let Some(resulting) = resulting else {
            return Ok(Outcome::confirm(
                payload,
                Some("Shuffle toggled".to_string()),
            ));
        };
        Ok(Outcome::confirm(
            payload,
            Some(format!("Shuffle: {}", if resulting { "on" } else { "off" })),
        ))
    }

    pub fn queue_repeat(&self, mode: Option<&str>) -> Result<Outcome> {
        let wire = match mode {
            Some("off") => Some(RepeatMode::Off),
            Some("one") => Some(RepeatMode::One),
            Some("all") => Some(RepeatMode::All),
            Some(other) => {
                return Ok(Outcome::fail(format!(
                    "repeat: invalid mode '{other}' (expected off|one|all)"
                )))
            }
            None => {
                let current = self
                    .send(&Command::StatusReq)
                    .ok()
                    .and_then(|payload| payload.status().map(|status| status.repeat));
                match current {
                    Some(RepeatMode::Off) => Some(RepeatMode::All),
                    Some(RepeatMode::All) => Some(RepeatMode::One),
                    Some(RepeatMode::One) => Some(RepeatMode::Off),
                    _ => return Ok(Outcome::fail("repeat: could not read current mode")),
                }
            }
        };
        let payload = self.send(&Command::QueueRepeat { mode: wire })?;
        let final_mode = payload
            .status()
            .map_or_else(|| wire.unwrap_or(RepeatMode::Off), |status| status.repeat);
        let label = match final_mode {
            RepeatMode::All => "all",
            RepeatMode::One => "one",
            _ => "off",
        };
        Ok(Outcome::confirm(payload, Some(format!("Repeat: {label}"))))
    }

    pub fn config_get(&self, key: &str) -> Result<Outcome> {
        // The daemon owns clamped/canonicalized values while it runs; with no
        // daemon the file is the only authority, so read it directly.
        if client::probe(&self.config.socket_path) {
            return self.confirm(
                &Command::ConfigGet {
                    key: key.to_string(),
                },
                None,
            );
        }
        let value = crate::config::get_raw(&self.config.config_path, key)?;
        let json = serde_json::json!({
            "type": "ConfigValue",
            "key": key,
            "value": value,
        });
        Ok(Outcome::confirm(
            Payload {
                json,
                reply: Reply::ConfigValue(ConfigValue {
                    key: key.to_string(),
                    value,
                }),
            },
            None,
        ))
    }

    pub fn config_set(&self, key: &str, value: &str) -> Result<Outcome> {
        self.confirm(
            &Command::ConfigSet {
                key: key.to_string(),
                value: value.to_string(),
            },
            None,
        )
    }

    pub fn config_list(&self) -> Result<Outcome> {
        self.confirm(&Command::ConfigList, None)
    }

    pub fn config_export(&self, path: &str) -> Result<Outcome> {
        self.confirm(
            &Command::ConfigExport {
                path: path.to_string(),
            },
            Some(format!("Exported config to {path}")),
        )
    }

    pub fn config_import(&self, path: &str) -> Result<Outcome> {
        self.confirm(
            &Command::ConfigImport {
                path: path.to_string(),
            },
            Some(format!("Imported config from {path}")),
        )
    }

    pub fn config_reset(&self, key: Option<&str>) -> Result<Outcome> {
        let line = key.map_or_else(
            || "Reset all config".to_string(),
            |key| format!("Reset config key '{key}'"),
        );
        self.confirm(
            &Command::ConfigReset {
                key: key.map(ToString::to_string),
            },
            Some(line),
        )
    }

    pub fn device_list(&self) -> Result<Outcome> {
        self.confirm(&Command::DeviceList, None)
    }

    pub fn device_set(&self, id: &str) -> Result<Outcome> {
        self.confirm(
            &Command::DeviceSet { id: id.to_string() },
            Some(format!("Default device: {id}")),
        )
    }

    pub fn device_test(&self, id: Option<&str>, tone: bool) -> Result<Outcome> {
        let mut line = id.map_or_else(
            || "Default device available".to_string(),
            |id| format!("Device available: {id}"),
        );
        if tone {
            line.push_str(" (playing test tone)");
        }
        self.confirm(
            &Command::DeviceTest {
                id: id.map(ToString::to_string),
                tone,
            },
            Some(line),
        )
    }

    pub fn playlist_list(&self) -> Result<Outcome> {
        self.confirm(&Command::PlaylistList, None)
    }

    pub fn playlist_tracks(&self, pid: i64) -> Result<Outcome> {
        self.confirm(&Command::PlaylistTracks { pid }, None)
    }

    pub fn playlist_create(&self, name: &str, smart: &str) -> Result<Outcome> {
        let payload = self.send(&Command::PlaylistCreate {
            name: name.to_string(),
            smart_query: smart.to_string(),
        })?;
        let line = match &payload.reply {
            Reply::PlaylistCreated { id, name } => Some(format!("Created playlist {id} '{name}'")),
            _ => None,
        };
        Ok(Outcome::confirm(payload, line))
    }

    /// Port of `App::playlistAdd`: resolve ids and paths to library tracks,
    /// append each one, and report per-track lines.
    pub fn playlist_add(
        &self,
        pid: i64,
        ids: &[String],
        paths: &[String],
        recursive: bool,
    ) -> BatchReport {
        if ids.is_empty() && paths.is_empty() {
            return BatchReport::fail("playlist add: need --id ID or PATH");
        }
        let mut resolved = Vec::new();
        for text in ids {
            let Some(id) = crate::parse::parse_id(text) else {
                return BatchReport::fail(format!(
                    "playlist add: --id needs a non-negative numeric id: {text}"
                ));
            };
            resolved.push(id);
        }
        let mut files = Vec::new();
        let mut unmatched = Vec::new();
        for token in paths {
            crate::paths::expand_add_token(token, recursive, &mut files, &mut unmatched);
        }
        let mut report = BatchReport::default();
        let mut hard_fail = false;
        for token in &unmatched {
            if crate::paths::is_numeric(token) {
                report.err_line(format!(
                    "playlist add: '{token}' is not a file (use --id for ids)"
                ));
                hard_fail = true;
            } else if crate::paths::has_glob_chars(token) || std::path::Path::new(token).is_dir() {
                report.err_line(format!("No files matched: {token}"));
            } else {
                report.err_line(format!("playlist add: no such file: {token}"));
                hard_fail = true;
            }
        }
        for file in &files {
            self.ensure_library_track(file, &mut report, &mut resolved, &mut hard_fail);
        }
        let (added, collected) =
            self.append_playlist_tracks(pid, resolved, &mut report, &mut hard_fail);
        report.json = Some(serde_json::json!({"type": "QueueTracks", "tracks": collected}));
        crate::batch::count_line_text(added, &mut report.out);
        report.exit_code = i32::from(added == 0 && hard_fail);
        report
    }

    /// Library-resolves one expanded path: `LibraryAdd`, then an exact
    /// `pathKey` match in a filename search (the C++ loop body).
    fn ensure_library_track(
        &self,
        file: &str,
        report: &mut BatchReport,
        resolved: &mut Vec<i64>,
        hard_fail: &mut bool,
    ) {
        if let Err(err) = self.send(&Command::LibraryAdd {
            path: file.to_string(),
            recursive: false,
        }) {
            report.err_block(&err.render());
            *hard_fail = true;
            return;
        }
        let search = match self.send(&Command::LibrarySearch {
            query: crate::paths::filename_of(file),
            limit: 50,
        }) {
            Ok(payload) => payload,
            Err(err) => {
                report.err_block(&err.render());
                *hard_fail = true;
                return;
            }
        };
        let Reply::SearchResults { tracks, .. } = &search.reply else {
            report.out_block(&crate::output::render_payload(&search, 0, false));
            *hard_fail = true;
            return;
        };
        let key = crate::paths::path_key(file);
        if let Some(track) = tracks
            .iter()
            .find(|track| crate::paths::path_key(&track.path) == key)
        {
            resolved.push(track.id);
        } else {
            report.err_line(format!("playlist add: not in library: {file}"));
            *hard_fail = true;
        }
    }

    /// Appends each resolved id and tags every success with the C++ label.
    fn append_playlist_tracks(
        &self,
        pid: i64,
        resolved: Vec<i64>,
        report: &mut BatchReport,
        hard_fail: &mut bool,
    ) -> (usize, Vec<serde_json::Value>) {
        let mut added = 0_usize;
        let mut collected = Vec::new();
        for track_id in resolved {
            match self.send(&Command::PlaylistAdd { pid, track_id }) {
                Err(err) if err.code == StatusCode::AlreadyExists => {
                    report.err_line(format!("already on playlist: track {track_id}"));
                    continue;
                }
                Err(err) => {
                    report.err_block(&err.render());
                    *hard_fail = true;
                    continue;
                }
                Ok(_) => {}
            }
            let mut label = String::new();
            if let Ok(payload) = self.send(&Command::TagGet {
                id: Some(track_id),
                path: None,
                cover: false,
            }) {
                if let Reply::SingleTrack { track, .. } = &payload.reply {
                    if let Some(raw) = payload.json.get("track") {
                        collected.push(raw.clone());
                    }
                    label = added_label(track);
                }
            }
            if label.is_empty() {
                label = format!("track {track_id}");
            }
            report.out_line(format!("Added {label} to playlist {pid}"));
            added += 1;
        }
        (added, collected)
    }

    pub fn playlist_load(&self, pid: i64, play: bool, replace: bool) -> Result<Outcome> {
        let payload = self.send(&Command::PlaylistLoad { pid, play, replace })?;
        let line = match &payload.reply {
            Reply::PlaylistLoaded { queue_id, status } => {
                let mut line = if replace {
                    format!("Replaced queue {queue_id} with playlist {pid}")
                } else {
                    format!("Loaded playlist {pid} into queue {queue_id}")
                };
                if play {
                    let _ = write!(line, "\nPlaying {}", track_who(status));
                }
                Some(line)
            }
            _ => None,
        };
        Ok(Outcome::confirm(payload, line))
    }

    pub fn playlist_save(&self, name: &str, queue_id: Option<i64>) -> Result<Outcome> {
        self.confirm(
            &Command::PlaylistSave {
                name: name.to_string(),
                queue_id,
            },
            Some(format!("Saved playlist '{name}'")),
        )
    }

    pub fn playlist_delete(&self, pid: i64) -> Result<Outcome> {
        self.confirm(
            &Command::PlaylistDelete { pid },
            Some(format!("Deleted playlist {pid}")),
        )
    }

    pub fn playlist_rename(&self, pid: i64, name: &str) -> Result<Outcome> {
        self.confirm(
            &Command::PlaylistRename {
                pid,
                new_name: name.to_string(),
            },
            Some(format!("Renamed playlist {pid} to '{name}'")),
        )
    }

    /// Port of `App::playlistExport`: the daemon returns the rows, the
    /// frontend writes the file (m3u/pls text or the kanariv JSON shape).
    #[must_use]
    pub fn playlist_export(&self, pid: i64, path: &str, format: &str) -> BatchReport {
        let payload = match self.send(&Command::PlaylistExport {
            pid,
            path: path.to_string(),
            format: format.to_string(),
        }) {
            Ok(payload) => payload,
            Err(err) => return BatchReport::fail(format!("export: {}", err.message)),
        };
        let Reply::PlaylistData { tracks, format } = &payload.reply else {
            return BatchReport::fail("export: unexpected response from daemon");
        };
        let text = match format.as_str() {
            "m3u" | "pls" => render_playlist_text(tracks, format),
            "json" => render_playlist_json(&payload),
            _ => String::new(),
        };
        if std::fs::write(path, native_newlines(text)).is_err() {
            return BatchReport::fail(format!("export: failed to open output file '{path}'"));
        }
        BatchReport {
            out: format!("exported {} tracks to {path}", tracks.len()),
            ..BatchReport::default()
        }
    }

    pub fn playlist_import(&self, path: &str, name: Option<&str>) -> Result<Outcome> {
        self.confirm(
            &Command::PlaylistImport {
                path: path.to_string(),
                name: name.map(ToString::to_string),
            },
            None,
        )
    }
}

/// `kanariv::app::addedLabel`: "artist - title: file", falling back by part.
#[must_use]
pub fn added_label(track: &Track) -> String {
    let filename = crate::paths::filename_of(&track.path);
    let who = if !track.artist.is_empty() && !track.title.is_empty() {
        format!("{} - {}", track.artist, track.title)
    } else {
        format!("{}{}", track.artist, track.title)
    };
    if !who.is_empty() && !filename.is_empty() {
        format!("{who}: {filename}")
    } else if !filename.is_empty() {
        filename
    } else if !who.is_empty() {
        who
    } else {
        format!("track {}", track.id)
    }
}

/// `writePlaylistText`: M3U and PLS bodies with `\n` line endings.
#[must_use]
pub fn render_playlist_text(tracks: &[Track], format: &str) -> String {
    let mut out = String::new();
    if format == "m3u" {
        out.push_str("#EXTM3U\n");
        for track in tracks {
            let duration = track.duration.round() as i64;
            let title = if track.title.is_empty() {
                track.path.as_str()
            } else {
                track.title.as_str()
            };
            let _ = write!(out, "#EXTINF:{duration},");
            if track.artist.is_empty() {
                out.push_str(title);
            } else {
                let _ = write!(out, "{} - {title}", track.artist);
            }
            out.push('\n');
            out.push_str(&track.path);
            out.push('\n');
        }
    } else if format == "pls" {
        out.push_str("[playlist]\n");
        for (index, track) in tracks.iter().enumerate() {
            let number = index + 1;
            let _ = writeln!(out, "File{number}={}", track.path);
            let title = if track.title.is_empty() {
                track.path.as_str()
            } else {
                track.title.as_str()
            };
            let title = if track.artist.is_empty() {
                title.to_string()
            } else {
                format!("{} - {title}", track.artist)
            };
            let _ = writeln!(out, "Title{number}={title}");
            let duration = track.duration.round() as i64;
            let _ = writeln!(out, "Length{number}={duration}");
        }
        let _ = writeln!(out, "NumberOfEntries={}", tracks.len());
        out.push_str("Version=2\n");
    }
    out
}

/// `writePlaylistJson`: the daemon's track objects under the kanariv header.
#[must_use]
pub fn render_playlist_json(payload: &Payload) -> String {
    let tracks = payload
        .json
        .get("tracks")
        .cloned()
        .unwrap_or_else(|| serde_json::Value::Array(Vec::new()));
    let mut root = serde_json::Map::new();
    root.insert(
        "format".to_string(),
        serde_json::Value::String("kanariv-playlist".to_string()),
    );
    root.insert("version".to_string(), serde_json::Value::from(1));
    root.insert("tracks".to_string(), tracks);
    let mut text = serde_json::to_string_pretty(&serde_json::Value::Object(root))
        .unwrap_or_else(|_| String::new());
    text.push('\n');
    text
}

/// `std::ofstream` text mode writes CRLF on Windows; the file parity target
/// is the C++ CLI's bytes, so reproduce that.
fn native_newlines(text: String) -> String {
    if cfg!(windows) {
        text.replace('\n', "\r\n")
    } else {
        text
    }
}

#[must_use]
pub fn track_who(status: &Status) -> String {
    if !status.artist.is_empty() && !status.title.is_empty() {
        return format!("{} - {}", status.artist, status.title);
    }
    if !status.artist.is_empty() {
        return status.artist.clone();
    }
    if !status.title.is_empty() {
        return status.title.clone();
    }
    if !status.path.is_empty() {
        if let Some(name) = std::path::Path::new(&status.path)
            .file_name()
            .and_then(|name| name.to_str())
        {
            if !name.is_empty() {
                return name.to_string();
            }
        }
    }
    "unknown track".to_string()
}

/// `kanariv::utils::formatClock` (mm:ss, or hh:mm:ss past an hour).
#[must_use]
pub fn format_clock(seconds: f64) -> String {
    let total = if seconds < 0.0 { 0 } else { seconds as i64 };
    let hours = total / 3600;
    if hours > 0 {
        format!("{hours:02}:{:02}:{:02}", (total % 3600) / 60, total % 60)
    } else {
        format!("{:02}:{:02}", (total % 3600) / 60, total % 60)
    }
}
