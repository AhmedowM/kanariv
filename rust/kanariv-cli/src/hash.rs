//! Socket/pid endpoint id, matching `config::detail::endpointIdForDb`.
//!
//! The daemon hashes the generic UTF-8 database path with FNV-1a 64-bit and
//! formats it as 16 lowercase hex digits. The hash is specified (not
//! `std::hash`), so the name is toolchain independent and reproduced here
//! byte-for-byte on every target.

const FNV_OFFSET: u64 = 0xcbf2_9ce4_8422_2325;
const FNV_PRIME: u64 = 0x0000_0100_0000_01b3;

#[must_use]
pub fn fnv1a64(bytes: &[u8]) -> u64 {
    let mut hash = FNV_OFFSET;
    for &byte in bytes {
        hash ^= u64::from(byte);
        hash = hash.wrapping_mul(FNV_PRIME);
    }
    hash
}

/// `{:016x}` of FNV-1a over the generic path, as `endpointIdForDb` formats it.
#[must_use]
pub fn endpoint_id_for_db(db_path_generic: &str) -> String {
    format!("{:016x}", fnv1a64(db_path_generic.as_bytes()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fnv_known_vectors() {
        assert_eq!(fnv1a64(b""), 0xcbf2_9ce4_8422_2325);
        assert_eq!(fnv1a64(b"a"), 0xaf63_dc4c_8601_ec8c);
        assert_eq!(fnv1a64(b"test"), 0xf9e6_e6ef_197c_2b25);
    }

    /// Same vectors as `tests/test_config.cpp`.
    #[test]
    fn endpoint_ids_match_the_cpp_derivation() {
        assert_eq!(endpoint_id_for_db("data/library.db"), "4f477ef70d3ca00b");
        assert_eq!(
            endpoint_id_for_db("C:/kanariv/library.db"),
            "1b00c40520a0a75b"
        );
        assert_eq!(endpoint_id_for_db(""), "cbf29ce484222325");
    }
}
