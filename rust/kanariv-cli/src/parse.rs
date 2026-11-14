//! Argument grammar for `seek` and `volume` (port of cli/src/shell/parse.hpp).
//!
//! Error strings are part of the CLI contract; the golden tests match on
//! fragments such as "invalid time" and "ss < 60".

#[derive(Debug, Clone, PartialEq)]
pub enum VolumeArg {
    Show,
    Set {
        level: Option<f32>,
        mute: Option<bool>,
        delta_pct: Option<i32>,
    },
}

pub fn parse_time(input: &str) -> Result<f64, String> {
    let text = input.trim_matches([' ', '\t', '\r', '\n']);
    if text.is_empty() {
        return Err("empty time".to_string());
    }

    if !text.contains(':') {
        let value = parse_double(text).ok_or_else(|| "invalid time".to_string())?;
        if !value.is_finite() || value < 0.0 {
            return Err("invalid time".to_string());
        }
        return Ok(value);
    }

    let parts: Vec<&str> = text.split(':').collect();
    if parts.len() > 3 {
        return Err("too many colons".to_string());
    }
    if parts.iter().any(|part| part.is_empty()) {
        return Err("empty component".to_string());
    }

    let mut values = [0.0f64; 3];
    for (index, part) in parts.iter().enumerate() {
        let is_last = index + 1 == parts.len();
        values[index] = if is_last {
            parse_double(part).ok_or_else(|| "invalid time component".to_string())?
        } else {
            let parsed: i64 = part
                .parse()
                .map_err(|_| "invalid time component".to_string())?;
            if parsed < 0 {
                return Err("negative component".to_string());
            }
            parsed as f64
        };
    }

    let total = if parts.len() == 2 {
        let (minutes, seconds) = (values[0], values[1]);
        if !(0.0..60.0).contains(&seconds) {
            return Err("seconds out of range (ss < 60)".to_string());
        }
        minutes * 60.0 + seconds
    } else {
        let (hours, minutes, seconds) = (values[0], values[1], values[2]);
        if !(0.0..60.0).contains(&minutes) {
            return Err("minutes out of range (mm < 60)".to_string());
        }
        if !(0.0..60.0).contains(&seconds) {
            return Err("seconds out of range (ss < 60)".to_string());
        }
        hours * 3600.0 + minutes * 60.0 + seconds
    };

    if !total.is_finite() || total < 0.0 {
        return Err("invalid time".to_string());
    }
    Ok(total)
}

/// Returns `(seconds, relative)`; a leading +/- marks a delta.
pub fn parse_seek(input: &str) -> Result<(f64, bool), String> {
    let text = input.trim_matches([' ', '\t']);
    if text.is_empty() {
        return Err("empty seek".to_string());
    }
    let mut relative = false;
    let mut negative = false;
    let mut core = text;
    if let Some(rest) = core.strip_prefix(['+', '-']) {
        relative = true;
        negative = core.starts_with('-');
        core = rest;
        if core.is_empty() {
            return Err("missing seek after sign".to_string());
        }
    }
    let value = parse_time(core)?;
    Ok((if relative && negative { -value } else { value }, relative))
}

pub fn parse_volume(input: &str) -> Result<VolumeArg, String> {
    let text = input.trim_matches([' ', '\t']);
    if text.is_empty() {
        return Ok(VolumeArg::Show);
    }
    if text == "mute" {
        return Ok(VolumeArg::Set {
            level: None,
            mute: Some(true),
            delta_pct: None,
        });
    }
    if text == "unmute" {
        return Ok(VolumeArg::Set {
            level: None,
            mute: Some(false),
            delta_pct: None,
        });
    }
    if let Some(rest) = text.strip_prefix(['+', '-']) {
        let negative = text.starts_with('-');
        let value = parse_digits(rest).ok_or_else(|| "invalid delta".to_string())?;
        let value = i32::try_from(value).map_err(|_| "invalid delta".to_string())?;
        return Ok(VolumeArg::Set {
            level: None,
            mute: None,
            delta_pct: Some(if negative { -value } else { value }),
        });
    }
    let value = parse_digits(text).ok_or_else(|| "invalid volume".to_string())?;
    let level = i32::try_from(value).map_err(|_| "out of range".to_string())?;
    if !(0..=100).contains(&level) {
        return Err("out of range".to_string());
    }
    Ok(VolumeArg::Set {
        level: Some(level as f32),
        mute: None,
        delta_pct: None,
    })
}

/// strtod-compatible strict token parse (no whitespace, no hex floats).
fn parse_double(token: &str) -> Option<f64> {
    if token.is_empty() || token.starts_with([' ', '\t', '\r', '\n']) || is_hex_float(token) {
        return None;
    }
    token.parse::<f64>().ok()
}

fn is_hex_float(token: &str) -> bool {
    let digits = token.strip_prefix(['+', '-']).unwrap_or(token);
    digits.len() > 2 && (digits.starts_with("0x") || digits.starts_with("0X"))
}

fn parse_digits(token: &str) -> Option<u64> {
    if token.is_empty() || !token.bytes().all(|byte| byte.is_ascii_digit()) {
        return None;
    }
    token.parse::<u64>().ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn time_accepts_all_forms() {
        assert_eq!(parse_time("95"), Ok(95.0));
        assert_eq!(parse_time("1:35"), Ok(95.0));
        assert_eq!(parse_time("1:02:03.5"), Ok(3723.5));
        assert_eq!(parse_time(" 5 "), Ok(5.0));
    }

    #[test]
    fn time_rejects_overflow_and_garbage() {
        assert_eq!(
            parse_time("1:60"),
            Err("seconds out of range (ss < 60)".into())
        );
        assert_eq!(
            parse_time("1:60:00"),
            Err("minutes out of range (mm < 60)".into())
        );
        assert_eq!(parse_time("1:2:3:4"), Err("too many colons".into()));
        assert_eq!(parse_time("::"), Err("empty component".into()));
        assert_eq!(parse_time("abc"), Err("invalid time".into()));
    }

    #[test]
    fn seek_detects_relative_signs() {
        assert_eq!(parse_seek("+5"), Ok((5.0, true)));
        assert_eq!(parse_seek("-1:30"), Ok((-90.0, true)));
        assert_eq!(parse_seek("42"), Ok((42.0, false)));
    }

    #[test]
    fn volume_forms() {
        assert_eq!(parse_volume(""), Ok(VolumeArg::Show));
        assert_eq!(
            parse_volume("42"),
            Ok(VolumeArg::Set {
                level: Some(42.0),
                mute: None,
                delta_pct: None
            })
        );
        assert_eq!(
            parse_volume("-7"),
            Ok(VolumeArg::Set {
                level: None,
                mute: None,
                delta_pct: Some(-7)
            })
        );
        assert_eq!(
            parse_volume("mute"),
            Ok(VolumeArg::Set {
                level: None,
                mute: Some(true),
                delta_pct: None
            })
        );
        assert_eq!(parse_volume("101"), Err("out of range".into()));
        assert_eq!(parse_volume("loud"), Err("invalid volume".into()));
        assert_eq!(parse_volume("+x"), Err("invalid delta".into()));
    }
}
