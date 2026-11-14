//! `std::hash`-compatible socket-name derivation.
//!
//! The daemon derives its socket/pid names from `std::hash<std::string>` of
//! the database path. That hash is implementation-defined, so the Rust CLI
//! reproduces the two implementations it can meet in the field:
//!
//! * windows-msvc targets: FNV-1a 64-bit (matches MSVC STL daemons).
//! * unix targets: `MurmurHash64A` with seed `0xc70f6907` (matches libstdc++
//!   daemons).
//!
//! A daemon built with a different C++ runtime than the Rust target's
//! assumption -- MinGW/libstdc++ on Windows, or MSVC STL on a GNU host --
//! derives a different pipe name; set `socketPath` explicitly in that case.
//!
//! libc++ (macOS) uses a `CityHash` variant that is not reproduced here; on
//! macOS `config.rs` refuses to derive a socket path and asks for an
//! explicit `socketPath` instead of silently targeting the wrong pipe.

const FNV_OFFSET: u64 = 0xcbf2_9ce4_8422_2325;
const FNV_PRIME: u64 = 0x0000_0100_0000_01b3;
#[cfg(any(not(windows), test))]
const MURMUR_M: u64 = 0xc6a4_a793_5bd1_e995;
#[cfg(any(not(windows), test))]
const MURMUR_R: u32 = 47;
#[cfg(any(not(windows), test))]
const MURMUR_SEED: u64 = 0xc70f_6907;

#[must_use]
pub fn fnv1a64(bytes: &[u8]) -> u64 {
    let mut hash = FNV_OFFSET;
    for &byte in bytes {
        hash ^= u64::from(byte);
        hash = hash.wrapping_mul(FNV_PRIME);
    }
    hash
}

#[cfg(any(not(windows), test))]
#[must_use]
pub fn murmur64a(bytes: &[u8], seed: u64) -> u64 {
    let len = bytes.len();
    let mut hash = seed ^ (len as u64).wrapping_mul(MURMUR_M);

    let mut chunks = bytes.chunks_exact(8);
    for chunk in &mut chunks {
        let mut k = u64::from_le_bytes(chunk.try_into().expect("8-byte chunk"));
        k = k.wrapping_mul(MURMUR_M);
        k ^= k >> MURMUR_R;
        k = k.wrapping_mul(MURMUR_M);
        hash ^= k;
        hash = hash.wrapping_mul(MURMUR_M);
    }

    let tail = chunks.remainder();
    if !tail.is_empty() {
        let mut value = 0u64;
        for (index, &byte) in tail.iter().enumerate() {
            value |= u64::from(byte) << (index * 8);
        }
        hash ^= value;
        hash = hash.wrapping_mul(MURMUR_M);
    }

    hash ^= hash >> MURMUR_R;
    hash = hash.wrapping_mul(MURMUR_M);
    hash ^= hash >> MURMUR_R;
    hash
}

/// `std::hash<std::string>` for the current target's standard library.
#[must_use]
pub fn std_string_hash(input: &str) -> u64 {
    #[cfg(windows)]
    {
        fnv1a64(input.as_bytes())
    }
    #[cfg(not(windows))]
    {
        murmur64a(input.as_bytes(), MURMUR_SEED)
    }
}

/// Reproduces `hex8ForDb`: fold the 64-bit hash to 32 bits, lowercase hex.
#[must_use]
pub fn hex8_for_db(db_path_generic: &str) -> String {
    let raw = std_string_hash(db_path_generic);
    let folded = ((raw & 0xffff_ffff) as u32) ^ (((raw >> 32) & 0xffff_ffff) as u32);
    format!("{folded:08x}")
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

    #[test]
    fn murmur_known_vectors() {
        assert_eq!(murmur64a(b"", MURMUR_SEED), 0x553e_9390_1e46_2a6e);
        assert_eq!(murmur64a(b"a", MURMUR_SEED), 0x454d_dee4_88c1_ed6b);
        assert_eq!(murmur64a(b"test", MURMUR_SEED), 0xd1d1_6a4a_0a7a_19fb);
    }

    #[test]
    fn hex8_folds_high_and_low_words() {
        assert_eq!(
            hex8_for_db(""),
            if cfg!(windows) {
                "4fd0bfc1"
            } else {
                "4b78b9fe"
            }
        );
        assert_eq!(
            hex8_for_db("test"),
            if cfg!(windows) {
                "e09acdca"
            } else {
                "dbab73b1"
            }
        );
        assert_eq!(
            hex8_for_db("/tmp/kanariv/library.db"),
            if cfg!(windows) {
                "daaf2057"
            } else {
                "19f1cd5d"
            }
        );
    }
}
