//! Guards the Rust schema against drift from `src/db/core.cpp`.
//!
//! The test extracts the concatenated `schema` string literal from the C++
//! source and compares it byte-for-byte with [`kanariv_db::SCHEMA`]. Editing
//! one side without the other fails here before any database is created.

use std::fs;
use std::path::PathBuf;

#[test]
fn schema_matches_cpp_source() {
    let cpp = extract_cpp_schema(&cpp_source());
    assert_eq!(
        kanariv_db::SCHEMA,
        cpp,
        "Rust schema drifted from src/db/core.cpp"
    );
}

fn cpp_source() -> String {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../src/db/core.cpp");
    fs::read_to_string(&path).unwrap_or_else(|err| panic!("read {}: {err}", path.display()))
}

/// Concatenates the C string literals that make up the `schema` constant.
fn extract_cpp_schema(source: &str) -> String {
    let marker = "inline constexpr std::string_view schema =";
    let start = source
        .find(marker)
        .unwrap_or_else(|| panic!("missing `{marker}`"))
        + marker.len();
    let bytes = &source.as_bytes()[start..];
    let mut out = String::new();
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b';' => break,
            b'"' => {
                i += 1;
                while i < bytes.len() && bytes[i] != b'"' {
                    if bytes[i] == b'\\' && i + 1 < bytes.len() {
                        i += 1;
                        out.push(match bytes[i] {
                            b'n' => '\n',
                            b't' => '\t',
                            b'r' => '\r',
                            other => char::from(other),
                        });
                    } else {
                        out.push(char::from(bytes[i]));
                    }
                    i += 1;
                }
                i += 1;
            }
            _ => i += 1,
        }
    }
    out
}
