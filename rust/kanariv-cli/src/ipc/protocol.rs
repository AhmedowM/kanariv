//! IPC command/result models matching `src/ipc/protocol.cpp`.
//!
//! Only the command subset exercised by this CLI slice is modelled; unknown
//! result types are preserved as `Payload::Other` so callers can dump them.

use serde::Deserialize;
use serde_json::{json, Value};

use crate::error::{Error, Result, StatusCode};

#[derive(Clone, Debug)]
pub enum Command {
    Play,
    Pause,
    Resume,
    Restart,
    Stop,
    Next,
    Prev,
    StatusReq,
    Shutdown,
    Info,
    Seek {
        seconds: f64,
    },
    VolumeSet {
        level: Option<f32>,
        mute: Option<bool>,
        delta_pct: Option<i32>,
    },
    QueueQueues,
    QueueList {
        order: String,
    },
    QueueCreate {
        name: String,
    },
    QueueDelete {
        qid: i64,
    },
    QueueSwitch {
        qid: i64,
    },
    QueueClear,
    QueueShuffle {
        on: Option<bool>,
    },
    QueueRepeat {
        mode: Option<&'static str>,
    },
}

impl Command {
    #[must_use]
    pub fn to_json(&self) -> Value {
        let type_name = |name: &str| json!({ "type": name });
        match self {
            Self::Play => type_name("Play"),
            Self::Pause => type_name("Pause"),
            Self::Resume => type_name("Resume"),
            Self::Restart => type_name("Restart"),
            Self::Stop => type_name("Stop"),
            Self::Next => type_name("Next"),
            Self::Prev => type_name("Prev"),
            Self::StatusReq => type_name("StatusReq"),
            Self::Shutdown => type_name("Shutdown"),
            Self::Info => type_name("Info"),
            Self::Seek { seconds } => json!({ "type": "Seek", "seconds": seconds }),
            Self::VolumeSet {
                level,
                mute,
                delta_pct,
            } => json!({
                "type": "VolumeSet",
                "level": level,
                "mute": mute,
                "deltaPct": delta_pct,
            }),
            Self::QueueQueues => type_name("QueueQueues"),
            Self::QueueList { order } => json!({ "type": "QueueList", "order": order }),
            Self::QueueCreate { name } => json!({ "type": "QueueCreate", "name": name }),
            Self::QueueDelete { qid } => json!({ "type": "QueueDelete", "qid": qid }),
            Self::QueueSwitch { qid } => json!({ "type": "QueueSwitch", "qid": qid }),
            Self::QueueClear => type_name("QueueClear"),
            Self::QueueShuffle { on } => json!({ "type": "QueueShuffle", "on": on }),
            Self::QueueRepeat { mode } => json!({ "type": "QueueRepeat", "mode": mode }),
        }
    }
}

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default)]
pub struct Track {
    pub id: i64,
    pub duration: f64,
    pub sample_rate: i64,
    pub channels: i64,
    pub bitrate: i64,
    pub year: i32,
    pub track_num: i32,
    pub disc_num: i32,
    pub path: String,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub album_artist: String,
    pub genre: String,
}

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default)]
pub struct Status {
    pub state: String,
    pub pos: f64,
    pub dur: f64,
    pub vol: f32,
    pub muted: bool,
    pub shuffle: bool,
    pub repeat: String,
    pub track_id: i64,
    pub title: String,
    pub artist: String,
    pub path: String,
    pub q_size: i64,
    pub q_idx: i64,
    pub version: String,
}

impl Status {
    #[must_use]
    pub fn is_paused(&self) -> bool {
        self.state == "Paused"
    }

    #[must_use]
    pub fn is_stopped(&self) -> bool {
        self.state == "Stopped"
    }

    #[must_use]
    pub fn is_playing(&self) -> bool {
        self.state == "Playing"
    }
}

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default)]
pub struct QueueEntry {
    pub id: i64,
    pub name: String,
    pub tracks: i64,
    pub active: bool,
    pub temp: bool,
}

#[derive(Clone, Debug)]
pub enum PayloadKind {
    Status(Status),
    QueueTracks(Vec<Track>),
    Queues(Vec<QueueEntry>),
    QueueCreated {
        id: i64,
        name: String,
    },
    VolumeInfo {
        vol: f32,
        muted: bool,
    },
    Info {
        track: Track,
        play_count: i64,
        last_played: i64,
    },
    Other,
}

/// A parsed result plus the original wire JSON (JSON mode re-emits it
/// verbatim so unknown fields survive).
#[derive(Clone, Debug)]
pub struct Payload {
    pub json: Value,
    pub kind: PayloadKind,
}

impl Payload {
    pub fn from_json(value: &Value) -> Result<Self> {
        let kind = match value
            .get("type")
            .and_then(Value::as_str)
            .unwrap_or_default()
        {
            "Status" => PayloadKind::Status(deserialize(value)?),
            "QueueTracks" => {
                PayloadKind::QueueTracks(deserialize(value.get("tracks").unwrap_or(&Value::Null))?)
            }
            "Queues" => {
                PayloadKind::Queues(deserialize(value.get("queues").unwrap_or(&Value::Null))?)
            }
            "QueueCreated" => PayloadKind::QueueCreated {
                id: value.get("id").and_then(Value::as_i64).unwrap_or_default(),
                name: value
                    .get("name")
                    .and_then(Value::as_str)
                    .unwrap_or_default()
                    .to_string(),
            },
            "VolumeInfo" => PayloadKind::VolumeInfo {
                vol: value.get("vol").and_then(Value::as_f64).unwrap_or(1.0) as f32,
                muted: value.get("muted").and_then(Value::as_bool).unwrap_or(false),
            },
            "TrackInfo" => PayloadKind::Info {
                track: deserialize(value.get("track").unwrap_or(&Value::Null))?,
                play_count: value
                    .get("play_count")
                    .and_then(Value::as_i64)
                    .unwrap_or_default(),
                last_played: value
                    .get("last_played")
                    .and_then(Value::as_i64)
                    .unwrap_or_default(),
            },
            _ => PayloadKind::Other,
        };
        Ok(Self {
            json: value.clone(),
            kind,
        })
    }

    #[must_use]
    pub fn status(&self) -> Option<&Status> {
        match &self.kind {
            PayloadKind::Status(status) => Some(status),
            _ => None,
        }
    }
}

fn deserialize<T: for<'de> Deserialize<'de>>(value: &Value) -> Result<T> {
    serde_json::from_value(value.clone())
        .map_err(|err| Error::new(StatusCode::Corrupt, format!("malformed reply: {err}")))
}

pub fn encode_request(id: u32, command: &Command) -> Value {
    json!({ "id": id, "cmd": command.to_json() })
}

/// Parses a reply frame into a payload, mapping `ok:false` to an error.
pub fn parse_reply(bytes: &[u8]) -> Result<Payload> {
    let json: Value = serde_json::from_slice(bytes)
        .map_err(|err| Error::new(StatusCode::Corrupt, format!("malformed reply: {err}")))?;
    let ok = json
        .get("ok")
        .and_then(Value::as_bool)
        .unwrap_or_else(|| json.get("error").is_none());
    if !ok {
        return Err(error_from_json(json.get("error").unwrap_or(&Value::Null)));
    }
    let Some(result) = json.get("result") else {
        return Err(Error::invalid_arg("missing result"));
    };
    Payload::from_json(result)
}

fn error_from_json(value: &Value) -> Error {
    let code = value
        .get("code")
        .and_then(Value::as_str)
        .map(StatusCode::from_wire)
        .or_else(|| {
            value
                .get("code_value")
                .and_then(Value::as_u64)
                .map(|v| StatusCode::from_value(u8::try_from(v).unwrap_or(255)))
        })
        .unwrap_or(StatusCode::Io);
    let message = value
        .get("message")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string();
    Error::new(code, message)
}
