//! `[u32 big-endian length][UTF-8 JSON]` framing with a 16 MiB cap.

use std::io::{self, Read};

use crate::error::{Result, WireError};

/// Same ceiling as the C++ `IpcClient::rawRecv`: larger frames are refused.
pub const MAX_FRAME: usize = 16 * 1024 * 1024;

/// Prefixes `payload` with its big-endian length.
///
/// # Errors
/// Returns `InvalidArg` when the payload exceeds [`MAX_FRAME`].
pub fn encode(payload: &[u8]) -> Result<Vec<u8>> {
    if payload.len() > MAX_FRAME {
        return Err(WireError::new(
            crate::error::StatusCode::InvalidArg,
            format!("frame too large: {} bytes", payload.len()),
        ));
    }
    let len = u32::try_from(payload.len())
        .map_err(|_| WireError::invalid_arg("frame length does not fit u32"))?;
    let mut out = Vec::with_capacity(payload.len() + 4);
    out.extend_from_slice(&len.to_be_bytes());
    out.extend_from_slice(payload);
    Ok(out)
}

/// Reads one framed payload, enforcing [`MAX_FRAME`] before allocating.
pub fn decode_from<R: Read>(reader: &mut R) -> io::Result<Vec<u8>> {
    let mut header = [0u8; 4];
    reader.read_exact(&mut header)?;
    let len = u32::from_be_bytes(header) as usize;
    if len > MAX_FRAME {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("frame too large: {len} bytes"),
        ));
    }
    let mut payload = vec![0u8; len];
    reader.read_exact(&mut payload)?;
    Ok(payload)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    #[test]
    fn round_trips_payload() {
        let framed = encode(b"{\"id\":1}").unwrap();
        assert_eq!(&framed[..4], &[0, 0, 0, 8]);
        assert_eq!(
            decode_from(&mut Cursor::new(framed)).unwrap(),
            b"{\"id\":1}"
        );
    }

    #[test]
    fn round_trips_empty_payload() {
        let framed = encode(b"").unwrap();
        assert_eq!(framed, [0, 0, 0, 0]);
        assert!(decode_from(&mut Cursor::new(framed)).unwrap().is_empty());
    }

    #[test]
    fn rejects_oversized_frame_on_decode() {
        let mut bytes = ((MAX_FRAME as u32) + 1).to_be_bytes().to_vec();
        bytes.extend_from_slice(&[0; 8]);
        let err = decode_from(&mut Cursor::new(bytes)).unwrap_err();
        assert_eq!(err.kind(), std::io::ErrorKind::InvalidData);
    }

    #[test]
    fn rejects_oversized_frame_on_encode() {
        let err = encode(&vec![0u8; MAX_FRAME + 1]).unwrap_err();
        assert_eq!(err.code, crate::error::StatusCode::InvalidArg);
    }

    #[test]
    fn truncated_frame_is_an_io_error() {
        let mut bytes = 16u32.to_be_bytes().to_vec();
        bytes.extend_from_slice(b"short");
        let err = decode_from(&mut Cursor::new(bytes)).unwrap_err();
        assert_eq!(err.kind(), std::io::ErrorKind::UnexpectedEof);
    }
}
