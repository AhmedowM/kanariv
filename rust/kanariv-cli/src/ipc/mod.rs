//! Wire protocol compatible with `kanariv::ipc` (serde models + framing).

pub mod frame;
pub mod protocol;
pub mod transport;

pub use frame::encode;
pub use protocol::{Payload, QueueEntry};
pub use transport::Connection;
