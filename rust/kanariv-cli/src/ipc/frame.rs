//! `[u32 big-endian length][UTF-8 JSON]` framing with a 16 MB cap.

use std::io::{self, Read};

/// Same ceiling as `IpcClient::rawRecv`: larger frames are a protocol error.
pub const MAX_FRAME: usize = 16 * 1024 * 1024;

#[must_use]
pub fn encode(payload: &[u8]) -> Vec<u8> {
    let len = u32::try_from(payload.len()).expect("payload fits u32");
    let mut out = Vec::with_capacity(payload.len() + 4);
    out.extend_from_slice(&len.to_be_bytes());
    out.extend_from_slice(payload);
    out
}

/// Reads one framed payload, enforcing [`MAX_FRAME`].
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
        let framed = encode(b"{\"id\":1}");
        assert_eq!(&framed[..4], &[0, 0, 0, 8]);
        assert_eq!(
            decode_from(&mut Cursor::new(framed)).unwrap(),
            b"{\"id\":1}"
        );
    }

    #[test]
    fn rejects_oversized_frame() {
        let mut bytes = ((MAX_FRAME as u32) + 1).to_be_bytes().to_vec();
        bytes.extend_from_slice(&[0; 8]);
        let err = decode_from(&mut Cursor::new(bytes)).unwrap_err();
        assert_eq!(err.kind(), std::io::ErrorKind::InvalidData);
    }
}
