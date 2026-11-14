//! Error type mirroring the status split used by the C++ database.

use thiserror::Error;

/// Database error categories.
///
/// Variants correspond to the C++ `StatusCode` values the persistence contract
/// documents for these operations.
#[derive(Debug, Error)]
pub enum DbError {
    #[error("invalid argument: {0}")]
    InvalidArg(String),
    #[error("not found: {0}")]
    NotFound(String),
    #[error("already exists: {0}")]
    AlreadyExists(String),
    #[error("database error: {0}")]
    Sqlite(#[from] rusqlite::Error),
}

impl DbError {
    #[must_use]
    pub fn invalid_arg(message: impl Into<String>) -> Self {
        Self::InvalidArg(message.into())
    }

    #[must_use]
    pub fn not_found(message: impl Into<String>) -> Self {
        Self::NotFound(message.into())
    }
}

/// Crate result alias.
pub type Result<T> = std::result::Result<T, DbError>;
