//! Multi-step batch outcome, mirroring `kanariv::app::BatchReport`.
//!
//! Batch handlers accumulate stdout/stderr text instead of printing; the
//! frontend flushes each stream and returns [`BatchReport::exit_code`].
//! `err` prints in both modes and `json` only under `--json`, exactly like
//! `detail::renderBatch`.

use serde_json::Value;

#[derive(Debug, Default)]
pub struct BatchReport {
    pub json: Option<Value>,
    pub out: String,
    pub err: String,
    pub exit_code: i32,
}

impl BatchReport {
    /// `BatchReport::fail`: stderr blob, no payload, exit 1.
    #[must_use]
    pub fn fail(message: impl Into<String>) -> Self {
        Self {
            err: message.into(),
            exit_code: 1,
            ..Self::default()
        }
    }

    pub fn out_line(&mut self, line: impl AsRef<str>) {
        push_line(&mut self.out, line.as_ref());
    }

    pub fn err_line(&mut self, line: impl AsRef<str>) {
        push_line(&mut self.err, line.as_ref());
    }

    pub fn out_block(&mut self, text: &str) {
        append_block(&mut self.out, text);
    }

    pub fn err_block(&mut self, text: &str) {
        append_block(&mut self.err, text);
    }
}

/// `detail::countLineText`: "1 track added" / "N tracks added".
pub fn count_line_text(added: usize, out: &mut String) {
    if added == 1 {
        push_line(out, "1 track added");
    } else {
        push_line(out, &format!("{added} tracks added"));
    }
}

fn push_line(blob: &mut String, line: &str) {
    if !blob.is_empty() {
        blob.push('\n');
    }
    blob.push_str(line);
}

/// `detail::appendBlock`: joins the non-final lines of `text` onto `blob`.
fn append_block(blob: &mut String, text: &str) {
    let mut start = 0_usize;
    let mut first = blob.is_empty();
    while start <= text.len() {
        let end = text[start..]
            .find('\n')
            .map_or(text.len(), |offset| start + offset);
        let line = &text[start..end];
        if end == text.len() && line.is_empty() {
            break;
        }
        if !first {
            blob.push('\n');
        }
        blob.push_str(line);
        first = false;
        start = end + 1;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lines_join_with_newlines() {
        let mut blob = String::new();
        push_line(&mut blob, "first");
        push_line(&mut blob, "second");
        assert_eq!(blob, "first\nsecond");
    }

    #[test]
    fn blocks_drop_one_trailing_newline() {
        let mut blob = String::new();
        append_block(&mut blob, "Error: State daemon not running\nhint: start\n");
        assert_eq!(blob, "Error: State daemon not running\nhint: start");
    }
}
