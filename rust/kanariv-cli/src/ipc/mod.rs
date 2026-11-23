//! Wire codec re-exports from the shared `kanariv-ipc` crate.

pub use kanariv_ipc::{
    encode, encode_request, parse_reply, Command, ConfigValue, Connection, DeviceInfo,
    LibraryStats, Payload, Playlist, QueueEntry, RepeatMode, Reply, Status, TagBatchEditEntry,
    TopEntity, Track,
};
