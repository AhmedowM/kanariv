//! Guards the Rust schema against drift from the C++ migration ladder.
//!
//! The base schema moved from `src/db/core.cpp` into `src/db/migration.cpp`
//! (`kBaseSchema`) when the versioned migrations landed. Every statement of
//! that constant must also appear in [`kanariv_db::SCHEMA`]: the crate may add
//! its own tables and pragmas, but it must not miss a C++ table, index or
//! trigger, because both sides open files the other created.

use std::fs;
use std::path::PathBuf;

#[test]
fn base_schema_statements_are_present() {
    let cpp = extract_literal(
        &cpp_source(),
        "inline constexpr std::string_view kBaseSchema =",
    );
    let rust = statements(kanariv_db::SCHEMA);
    for statement in statements(&cpp) {
        assert!(
            rust.contains(&statement),
            "Rust schema is missing the C++ statement: {statement}"
        );
    }
}

fn cpp_source() -> String {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../src/db/migration.cpp");
    fs::read_to_string(&path).unwrap_or_else(|err| panic!("read {}: {err}", path.display()))
}

/// Splits SQL into whitespace-normalised statements.
fn statements(sql: &str) -> Vec<String> {
    sql.split(';')
        .map(|statement| statement.split_whitespace().collect::<Vec<_>>().join(" "))
        .filter(|statement| !statement.is_empty())
        .collect()
}

/// Concatenates the C string literals that make up a `std::string_view`
/// constant starting at `marker` and ending at the next `;`.
fn extract_literal(source: &str, marker: &str) -> String {
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
