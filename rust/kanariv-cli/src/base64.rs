//! RFC 4648 base64 decoding, mirroring `kanariv::utils::Base64::decode`.
//!
//! The daemon delivers cover art as a base64 string and `tag get <id> cover
//! --out FILE` writes it back verbatim, so the accepted inputs and the exact
//! rejection messages have to match the C++ helper byte for byte.

use crate::error::{Error, Result};

/// The C++ header's table: 64 alphabet entries, -1 everywhere else.
const DECODE_TABLE: [i8; 256] = build_table();

const fn build_table() -> [i8; 256] {
    let alphabet = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut table = [-1_i8; 256];
    let mut index = 0_usize;
    while index < 64 {
        table[alphabet[index] as usize] = index as i8;
        index += 1;
    }
    table
}

fn decode_char(byte: u8) -> i8 {
    DECODE_TABLE[usize::from(byte)]
}

/// `std::isspace` on an unsigned char, i.e. the six C-locale whitespace bytes.
fn is_ascii_space(byte: u8) -> bool {
    matches!(byte, b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r')
}

/// `Base64::decode`: padding optional, ASCII whitespace ignored, and every
/// rejection carries the C++ message text.
pub fn decode(encoded: &str) -> Result<Vec<u8>> {
    let clean: Vec<u8> = encoded
        .bytes()
        .filter(|byte| !is_ascii_space(*byte))
        .collect();
    if clean.is_empty() {
        return Ok(Vec::new());
    }
    if clean.len() % 4 != 0 {
        return Err(Error::invalid_arg("base64: invalid encoded length"));
    }
    let mut pad_count = 0_usize;
    while pad_count < clean.len() && clean[clean.len() - 1 - pad_count] == b'=' {
        pad_count += 1;
    }
    if pad_count > 2 {
        return Err(Error::invalid_arg("base64: too many padding characters"));
    }
    let data_end = clean.len() - pad_count;
    if clean[..data_end].contains(&b'=') {
        return Err(Error::invalid_arg(
            "base64: padding character in middle of string",
        ));
    }
    // RFC 4648 section 3.5: the bits the padding covers must be zero.
    if pad_count > 0 && data_end > 0 {
        let last = decode_char(clean[data_end - 1]);
        let unused_mask = if pad_count == 1 { 0x03 } else { 0x0F };
        if last < 0 || (last & unused_mask) != 0 {
            return Err(Error::invalid_arg("base64: non-zero padding bits"));
        }
    }

    let mut out: Vec<u8> = Vec::with_capacity((clean.len() / 4) * 3 - pad_count);
    let mut index = 0_usize;
    while index < clean.len() {
        let mut buffer = 0_u32;
        let mut valid_chars = 0_usize;
        for offset in 0..4 {
            let byte = clean[index + offset];
            if byte == b'=' {
                break;
            }
            let value = decode_char(byte);
            if value < 0 {
                return Err(Error::invalid_arg(format!(
                    "base64: invalid character '{}'",
                    char::from(byte)
                )));
            }
            buffer = (buffer << 6) | u32::from((value & 0x3F) as u8);
            valid_chars += 1;
        }
        if valid_chars < 2 {
            return Err(Error::invalid_arg("base64: invalid encoded length"));
        }
        // Align the significant bits to the top of the 24-bit buffer.
        buffer <<= (4 - valid_chars) * 6;
        out.push(((buffer >> 16) & 0xFF) as u8);
        if valid_chars >= 3 {
            out.push(((buffer >> 8) & 0xFF) as u8);
        }
        if valid_chars == 4 {
            out.push((buffer & 0xFF) as u8);
        }
        index += 4;
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_the_known_vectors() {
        assert_eq!(decode("aGVsbG8=").unwrap(), b"hello");
        assert_eq!(decode("aGk=").unwrap(), b"hi");
        assert_eq!(decode("Sg==").unwrap(), b"J");
        assert_eq!(decode("aGVsbG8gd29ybGQ=").unwrap(), b"hello world");
        assert_eq!(decode("").unwrap(), Vec::<u8>::new());
    }

    #[test]
    fn ignores_ascii_whitespace() {
        assert_eq!(decode("aGVs bG8=\n").unwrap(), b"hello");
        assert_eq!(decode("\taGVsbG8=\r\n").unwrap(), b"hello");
    }

    #[test]
    fn rejects_like_the_cpp_helper() {
        assert_eq!(
            decode("abc").unwrap_err().message,
            "base64: invalid encoded length"
        );
        assert_eq!(
            decode("a===").unwrap_err().message,
            "base64: too many padding characters"
        );
        assert_eq!(
            decode("a=b=").unwrap_err().message,
            "base64: padding character in middle of string"
        );
        assert_eq!(
            decode("ab==").unwrap_err().message,
            "base64: non-zero padding bits"
        );
        assert_eq!(
            decode("a!bc").unwrap_err().message,
            "base64: invalid character '!'"
        );
    }
}
