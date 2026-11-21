//! Path, glob and file-expansion helpers shared by batch commands.
//! Mirrors `src/app/paths.cpp`.

use std::path::{Component, Path, PathBuf};

use crate::config::generic_string;

const AUDIO_EXTENSIONS: [&str; 15] = [
    ".mp3", ".flac", ".ogg", ".opus", ".oga", ".wav", ".m4a", ".mp4", ".wma", ".asf", ".aiff",
    ".aif", ".ape", ".mpc", ".wv",
];

/// `kanariv::media::hasAudioExt` (decode + tag-write extension set).
#[must_use]
pub fn has_audio_extension(path: &Path) -> bool {
    path.extension()
        .and_then(|ext| ext.to_str())
        .is_some_and(|ext| {
            let dotted = format!(".{}", ext.to_lowercase());
            AUDIO_EXTENSIONS.contains(&dotted.as_str())
        })
}

/// `kanariv::app::hasGlobChars`.
#[must_use]
pub fn has_glob_chars(text: &str) -> bool {
    text.contains(['*', '?'])
}

/// `kanariv::app::isNumeric` (the shared id grammar).
#[must_use]
pub fn is_numeric(text: &str) -> bool {
    crate::parse::parse_id(text).is_some()
}

/// Case-insensitive `*`/`?` wildcard match over bytes (C locale).
#[must_use]
pub fn wildcard_match(pattern: &str, name: &str) -> bool {
    let pattern = pattern.as_bytes();
    let name = name.as_bytes();
    let (mut px, mut nx) = (0_usize, 0_usize);
    let mut star = None;
    let mut star_next = 0_usize;
    while nx < name.len() {
        if px < pattern.len()
            && (pattern[px] == b'?' || pattern[px].eq_ignore_ascii_case(&name[nx]))
        {
            px += 1;
            nx += 1;
        } else if px < pattern.len() && pattern[px] == b'*' {
            star = Some(px);
            px += 1;
            star_next = nx;
        } else if let Some(star_px) = star {
            px = star_px + 1;
            star_next += 1;
            nx = star_next;
        } else {
            return false;
        }
    }
    while px < pattern.len() && pattern[px] == b'*' {
        px += 1;
    }
    px == pattern.len()
}

/// `kanariv::app::expandAddToken`: globs, folders (optionally recursive) and
/// plain files become absolute generic paths; tokens that match nothing go
/// to `unmatched`.
pub fn expand_add_token(
    token: &str,
    recursive: bool,
    files: &mut Vec<String>,
    unmatched: &mut Vec<String>,
) {
    if has_glob_chars(token) {
        let path = Path::new(token);
        let dir = match path.parent() {
            Some(parent) if !parent.as_os_str().is_empty() => parent.to_path_buf(),
            _ => PathBuf::from("."),
        };
        let pattern = path
            .file_name()
            .map_or_else(String::new, |name| name.to_string_lossy().into_owned());
        let hits = std::fs::read_dir(&dir).map_or_else(
            |_| Vec::new(),
            |entries| {
                entries
                    .flatten()
                    .filter(is_regular_file)
                    .map(|entry| entry.path())
                    .filter(|path| has_audio_extension(path))
                    .filter(|path| {
                        path.file_name()
                            .is_some_and(|name| wildcard_match(&pattern, &name.to_string_lossy()))
                    })
                    .map(|path| absolute_generic(&path))
                    .collect()
            },
        );
        push_hits(hits, token, files, unmatched);
        return;
    }
    let path = Path::new(token);
    if path.is_dir() {
        let mut hits = Vec::new();
        if recursive {
            walk_audio(path, &mut hits);
        } else if let Ok(entries) = std::fs::read_dir(path) {
            for entry in entries.flatten() {
                if !is_regular_file(&entry) {
                    continue;
                }
                let entry_path = entry.path();
                if has_audio_extension(&entry_path) {
                    hits.push(absolute_generic(&entry_path));
                }
            }
        }
        push_hits(hits, token, files, unmatched);
        return;
    }
    if path.is_file() {
        files.push(absolute_generic(path));
        return;
    }
    unmatched.push(token.to_string());
}

/// `kanariv::app::pathKey`: absolute, lexically normalized, generic spelling
/// and case-folded on Windows.
#[must_use]
pub fn path_key(path: &str) -> String {
    let path = Path::new(path);
    let absolute = if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir().map_or_else(|_| path.to_path_buf(), |cwd| cwd.join(path))
    };
    let mut key = generic_string(&lexically_normal(&absolute));
    if cfg!(windows) {
        key.make_ascii_lowercase();
    }
    key
}

/// `pathToUtf8(pathFromUtf8(path).filename())`.
#[must_use]
pub fn filename_of(path: &str) -> String {
    Path::new(path)
        .file_name()
        .map_or_else(String::new, |name| name.to_string_lossy().into_owned())
}

fn push_hits(
    mut hits: Vec<String>,
    token: &str,
    files: &mut Vec<String>,
    unmatched: &mut Vec<String>,
) {
    if hits.is_empty() {
        unmatched.push(token.to_string());
        return;
    }
    hits.sort();
    files.extend(hits);
}

fn is_regular_file(entry: &std::fs::DirEntry) -> bool {
    entry
        .file_type()
        .map_or_else(|_| entry.path().is_file(), |kind| kind.is_file())
}

fn walk_audio(root: &Path, hits: &mut Vec<String>) {
    let Ok(entries) = std::fs::read_dir(root) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let Ok(kind) = entry.file_type() else {
            continue;
        };
        if kind.is_dir() {
            walk_audio(&path, hits);
        } else if kind.is_file() && has_audio_extension(&path) {
            hits.push(absolute_generic(&path));
        }
    }
}

/// `std::filesystem::absolute(path).generic_string()`: purely lexical.
#[must_use]
pub fn absolute_generic(path: &Path) -> String {
    if path.is_absolute() {
        return generic_string(path);
    }
    std::env::current_dir().map_or_else(
        |_| generic_string(path),
        |cwd| generic_string(&cwd.join(path)),
    )
}

fn lexically_normal(path: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for component in path.components() {
        match component {
            Component::CurDir => {}
            Component::ParentDir => {
                out.pop();
            }
            other => out.push(other.as_os_str()),
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wildcard_matches_case_insensitively() {
        assert!(wildcard_match("*.WAV", "Sample.wav"));
        assert!(wildcard_match("sam?le.*", "Sample.wav"));
        assert!(!wildcard_match("*.mp3", "sample.wav"));
        assert!(wildcard_match("*", "anything"));
        assert!(!wildcard_match("a*", "ba"));
    }

    #[test]
    fn audio_extensions_are_case_insensitive() {
        assert!(has_audio_extension(Path::new("x.FLAC")));
        assert!(!has_audio_extension(Path::new("x.m3u")));
    }

    #[test]
    fn path_key_normalizes_separators() {
        let key = path_key("./tests/./fixtures/sample.wav");
        assert!(key.ends_with("/tests/fixtures/sample.wav"));
        if cfg!(windows) {
            assert!(!key.chars().any(char::is_uppercase));
        }
    }

    #[test]
    fn numeric_tokens_follow_the_id_grammar() {
        assert!(is_numeric("42"));
        assert!(!is_numeric("-1"));
        assert!(!is_numeric("1.5"));
        assert!(!is_numeric("99999999999999999999999"));
    }
}
