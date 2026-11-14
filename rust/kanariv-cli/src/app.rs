//! Playback/queue handlers, mirroring `kanariv::app`.
//!
//! Rendering stays out of here: handlers return payload + optional line,
//! exactly like the C++ `Outcome` contract.

use std::thread;
use std::time::Duration;

use crate::client::{self, DEFAULT_TIMEOUT};
use crate::config::Config;
use crate::error::{Result, StatusCode};
use crate::ipc::{Command, Payload, RepeatMode, Reply, Status};
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
