//! Minimal read-only C ABI over `kanariv-db`, used only by the FFI pilot.
//!
//! Exposes open/close, search, and get-track. Every returned string is
//! heap-allocated UTF-8 owned by the caller and must be released with
//! [`kanariv_db_ffi_string_free`]. Status codes are stable for this pilot:
//! 0 = ok, 1 = invalid argument, 2 = not found, 3 = internal.
//!
//! The daemon does not link this library; it exists to measure FFI cost and
//! ABI maintenance against the C++ database. The audio path is untouched.

use std::ffi::{c_char, CStr, CString};
use std::fmt::Write as _;
use std::ptr;

use kanariv_db::{DbError, Store, TrackRow};
use serde_json::{json, Value};

/// Opaque handle owned by the C caller.
pub struct KanarivDbHandle {
    store: Store,
}

const STATUS_OK: i32 = 0;
const STATUS_INVALID_ARG: i32 = 1;
const STATUS_NOT_FOUND: i32 = 2;
const STATUS_INTERNAL: i32 = 3;

fn status_for(error: &DbError) -> i32 {
    match error {
        DbError::InvalidArg(_) => STATUS_INVALID_ARG,
        DbError::NotFound(_) => STATUS_NOT_FOUND,
        DbError::AlreadyExists(_) | DbError::Sqlite(_) => STATUS_INTERNAL,
    }
}

fn c_string(text: &str) -> *mut c_char {
    let cleaned = text.replace('\0', " ");
    CString::new(cleaned).map_or(ptr::null_mut(), CString::into_raw)
}

fn set_out(slot: *mut *mut c_char, value: *mut c_char) {
    if !slot.is_null() {
        // SAFETY: the slot is an out-parameter supplied by the C caller and is
        // writable for the duration of the call, per the header contract.
        unsafe { *slot = value };
    }
}

fn set_error(slot: *mut *mut c_char, message: &str) {
    set_out(slot, c_string(message));
}

fn take_text(value: *const c_char) -> Option<String> {
    if value.is_null() {
        return None;
    }
    // SAFETY: the caller guarantees a NUL-terminated string that stays valid
    // for the duration of the call.
    let raw = unsafe { CStr::from_ptr(value) };
    raw.to_str().ok().map(str::to_owned)
}

fn fingerprint_hex(bytes: &[u8; 32]) -> String {
    let mut out = String::with_capacity(64);
    for byte in bytes {
        let _ = write!(out, "{byte:02x}");
    }
    out
}

fn track_json(track: &TrackRow) -> Value {
    json!({
        "id": track.id,
        "path": track.path,
        "title": track.title,
        "artist": track.artist,
        "album": track.album,
        "album_artist": track.album_artist,
        "genre": track.genre,
        "duration": track.duration,
        "library_id": track.library_id,
        "date_added": track.date_added,
        "play_count": track.play_count,
        "last_played": track.last_played,
        "sample_rate": track.sample_rate,
        "channels": track.channels,
        "bitrate": track.bitrate,
        "year": track.year,
        "track_num": track.track_num,
        "disc_num": track.disc_num,
        "rating": track.rating,
        "fingerprint": fingerprint_hex(&track.fingerprint),
    })
}

/// Opens (or creates) a database file.
///
/// # Safety
///
/// `path` must be a NUL-terminated string; `error` may be null or must point
/// to a writable `char*` slot that the caller later releases with
/// [`kanariv_db_ffi_string_free`]. Returns null on failure.
#[no_mangle]
pub unsafe extern "C" fn kanariv_db_ffi_open(
    path: *const c_char,
    error: *mut *mut c_char,
) -> *mut KanarivDbHandle {
    set_out(error, ptr::null_mut());
    let Some(path) = take_text(path) else {
        set_error(error, "path is null or not valid UTF-8");
        return ptr::null_mut();
    };
    match Store::open(&path) {
        Ok(store) => Box::into_raw(Box::new(KanarivDbHandle { store })),
        Err(err) => {
            set_error(error, &err.to_string());
            ptr::null_mut()
        }
    }
}

/// Releases a handle returned by [`kanariv_db_ffi_open`]. Null is ignored.
///
/// # Safety
///
/// `handle` must be null or an un-released pointer from `kanariv_db_ffi_open`.
#[no_mangle]
pub unsafe extern "C" fn kanariv_db_ffi_close(handle: *mut KanarivDbHandle) {
    if !handle.is_null() {
        // SAFETY: ownership of the box was transferred to the caller by open.
        drop(unsafe { Box::from_raw(handle) });
    }
}

/// Runs the three-stage search and writes a JSON array of tracks.
///
/// # Safety
///
/// `handle` must come from [`kanariv_db_ffi_open`]; `query` must be a valid
/// NUL-terminated string; `json` and `error` must be null or writable slots.
/// On success `*json` is owned by the caller (release with
/// [`kanariv_db_ffi_string_free`]).
#[no_mangle]
pub unsafe extern "C" fn kanariv_db_ffi_search_json(
    handle: *const KanarivDbHandle,
    query: *const c_char,
    limit: i32,
    json_out: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    set_out(json_out, ptr::null_mut());
    set_out(error, ptr::null_mut());
    // SAFETY: handle is either null or a live pointer from open (caller contract).
    let Some(handle) = (unsafe { handle.as_ref() }) else {
        set_error(error, "null handle");
        return STATUS_INVALID_ARG;
    };
    if json_out.is_null() {
        set_error(error, "null output slot");
        return STATUS_INVALID_ARG;
    }
    let Some(query) = take_text(query) else {
        set_error(error, "query is null or not valid UTF-8");
        return STATUS_INVALID_ARG;
    };
    match handle.store.search_tracks(&query, i64::from(limit)) {
        Ok(tracks) => {
            let values: Vec<Value> = tracks.iter().map(track_json).collect();
            match serde_json::to_string(&values) {
                Ok(text) => {
                    set_out(json_out, c_string(&text));
                    STATUS_OK
                }
                Err(err) => {
                    set_error(error, &err.to_string());
                    STATUS_INTERNAL
                }
            }
        }
        Err(err) => {
            set_error(error, &err.to_string());
            status_for(&err)
        }
    }
}

/// Loads one track as a JSON object.
///
/// # Safety
///
/// Same pointer and ownership contract as [`kanariv_db_ffi_search_json`].
#[no_mangle]
pub unsafe extern "C" fn kanariv_db_ffi_get_track_json(
    handle: *const KanarivDbHandle,
    id: i64,
    json_out: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    set_out(json_out, ptr::null_mut());
    set_out(error, ptr::null_mut());
    // SAFETY: handle is either null or a live pointer from open (caller contract).
    let Some(handle) = (unsafe { handle.as_ref() }) else {
        set_error(error, "null handle");
        return STATUS_INVALID_ARG;
    };
    if json_out.is_null() {
        set_error(error, "null output slot");
        return STATUS_INVALID_ARG;
    }
    if id <= 0 {
        set_error(error, "track id must be positive");
        return STATUS_INVALID_ARG;
    }
    match handle.store.get_track(id) {
        Ok(track) => match serde_json::to_string(&track_json(&track)) {
            Ok(text) => {
                set_out(json_out, c_string(&text));
                STATUS_OK
            }
            Err(err) => {
                set_error(error, &err.to_string());
                STATUS_INTERNAL
            }
        },
        Err(err) => {
            set_error(error, &err.to_string());
            status_for(&err)
        }
    }
}

/// Frees a string returned by this library. Null is ignored.
///
/// # Safety
///
/// `value` must be null or a pointer previously returned by this library and
/// not already released.
#[no_mangle]
pub unsafe extern "C" fn kanariv_db_ffi_string_free(value: *mut c_char) {
    if !value.is_null() {
        // SAFETY: the pointer came from CString::into_raw above and is freed once.
        drop(unsafe { CString::from_raw(value) });
    }
}
