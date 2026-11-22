//! Request/response client with the daemon's framing and timeout policy.

use std::sync::mpsc;
use std::time::Duration;

use crate::error::{Error, Result, StatusCode};
use crate::ipc::{encode, encode_request, parse_reply, Command, Connection, Payload};

pub const DEFAULT_TIMEOUT: Duration = Duration::from_millis(2000);
pub const BATCH_TIMEOUT: Duration = Duration::from_secs(5 * 60);
pub const LONG_TIMEOUT: Duration = Duration::from_secs(30 * 60);

/// `kanariv::ipc::transportPolicyFor`: the deadline class attached to each
/// command's schema row. Unlisted commands are Interactive.
#[must_use]
pub fn deadline_for(command: &Command) -> Duration {
    match command {
        Command::QueueAddBatch { .. }
        | Command::QueueRemoveBatch { .. }
        | Command::PlaylistExport { .. }
        | Command::PlaylistImport { .. }
        | Command::LibraryAdd { .. }
        | Command::LibraryMaintenance { .. }
        | Command::TagBatchEdit { .. } => BATCH_TIMEOUT,
        Command::LibraryScan { .. } | Command::LibraryAnalyze { .. } => LONG_TIMEOUT,
        _ => DEFAULT_TIMEOUT,
    }
}

/// Connect-only check (used for readiness and teardown polling).
#[must_use]
pub fn probe(socket_path: &str) -> bool {
    Connection::connect(socket_path).is_ok()
}

/// Sends one command and waits for its reply.
///
/// A failed connect is reported as `State/"daemon not running"` (the
/// transport detail is uninteresting to users and would break parity). The
/// exchange runs on a worker thread so a wedged peer surfaces `Io/"timeout"`
/// instead of blocking forever, mirroring `Client::send`.
pub fn send(socket_path: &str, command: &Command, timeout: Duration) -> Result<Payload> {
    let (sender, receiver) = mpsc::channel();
    let socket = socket_path.to_string();
    let command = command.clone();
    std::thread::spawn(move || {
        let result = transact(&socket, &command);
        let _ = sender.send(result);
    });

    match receiver.recv_timeout(timeout) {
        Ok(result) => result,
        Err(mpsc::RecvTimeoutError::Timeout | mpsc::RecvTimeoutError::Disconnected) => {
            Err(Error::new(StatusCode::Timeout, "timeout"))
        }
    }
}

fn transact(socket_path: &str, command: &Command) -> Result<Payload> {
    let mut connection = Connection::connect(socket_path)
        .map_err(|_| Error::new(StatusCode::State, "daemon not running"))?;
    let request = encode_request(1, command)?;
    let payload = serde_json::to_vec(&request)
        .map_err(|err| Error::new(StatusCode::Corrupt, err.to_string()))?;
    connection
        .send_bytes(&encode(&payload)?)
        .map_err(|err| Error::io(format!("send failed: {err}")))?;
    let reply = connection
        .recv_bytes()
        .map_err(|err| Error::io(format!("recv failed: {err}")))?;
    parse_reply(&reply).map_err(Error::from)
}
