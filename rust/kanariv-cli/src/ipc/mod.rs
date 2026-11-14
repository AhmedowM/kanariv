//! Wire codec re-exports from the shared `kanariv-ipc` crate.

pub use kanariv_ipc::{
    encode, encode_request, parse_reply, Command, Connection, Payload, QueueEntry, RepeatMode,
    Reply, Status, Track,
};
