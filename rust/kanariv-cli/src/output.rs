//! Human renderer matching `cli/src/shell/output_formatter.cpp`.
//!
//! String widths are measured in bytes, like `std::format` on `std::string`,
//! so table alignment matches the C++ output exactly for byte-oriented text.

use crate::ipc::protocol::{Payload, PayloadKind, Status, Track};

#[must_use]
pub fn format_time(seconds: f64) -> String {
    let total = if seconds < 0.0 { 0 } else { seconds as i64 };
    let hours = total / 3600;
    if hours > 0 {
        format!("{hours:02}:{:02}:{:02}", (total % 3600) / 60, total % 60)
    } else {
        format!("{:02}:{:02}", (total % 3600) / 60, total % 60)
    }
}

#[must_use]
pub fn truncate_field(text: &str, max_len: usize) -> String {
    if text.is_empty() {
        return "---".to_string();
    }
    if text.len() <= max_len {
        return text.to_string();
    }
    if max_len <= 3 {
        return String::from_utf8_lossy(&text.as_bytes()[..max_len]).into_owned();
    }
    format!(
        "{}...",
        String::from_utf8_lossy(&text.as_bytes()[..max_len - 3])
    )
}

fn pad_right(text: &str, width: usize) -> String {
    let mut out = text.to_string();
    while out.len() < width {
        out.push(' ');
    }
    out
}

fn pad_left(text: &str, width: usize) -> String {
    let mut out = String::new();
    while out.len() + text.len() < width {
        out.push(' ');
    }
    out.push_str(text);
    out
}

pub fn print_json(payload: &Payload) {
    match serde_json::to_string_pretty(&payload.json) {
        Ok(text) => println!("{text}"),
        Err(err) => eprintln!("Error: Corrupt {err}"),
    }
}

/// Renders a result in human form. `highlight` is the id to mark in a queue
/// listing (0 = none), matching the C++ formatter's `> ` marker.
pub fn print_payload(payload: &Payload, highlight: i64, color: bool) {
    match &payload.kind {
        PayloadKind::Status(status) => print_status(status),
        PayloadKind::QueueTracks(tracks) => print_queue_tracks(tracks, highlight, color),
        PayloadKind::Queues(entries) => print_queues(entries),
        PayloadKind::QueueCreated { id, name } => println!("Created queue {id} '{name}'"),
        PayloadKind::VolumeInfo { vol, muted } => print_volume(*vol, *muted),
        PayloadKind::Info {
            track,
            play_count,
            last_played,
        } => print_track_info(track, *play_count, *last_played),
        PayloadKind::Other => print_json(payload),
    }
}

pub fn print_status(status: &Status) {
    println!("State: {}", status.state);
    println!(
        "Pos: {} / {}",
        format_time(status.pos),
        format_time(status.dur)
    );
    if status.muted {
        println!("Volume: muted");
    } else {
        println!("Volume: {}%", (status.vol * 100.0_f32) as i32);
    }
    println!(
        "Shuffle: {} Repeat: {}",
        if status.shuffle { "on" } else { "off" },
        status.repeat
    );
    if !status.artist.is_empty() && !status.title.is_empty() {
        println!(
            "Track: {} - {} [id: {}]",
            truncate_field(&status.artist, 40),
            truncate_field(&status.title, 40),
            status.track_id
        );
    } else if !status.artist.is_empty() || !status.title.is_empty() {
        println!(
            "Track: {} [id: {}]",
            truncate_field(&(status.artist.clone() + &status.title), 40),
            status.track_id
        );
    } else if status.track_id != 0 {
        println!("Track: [id: {}]", status.track_id);
    }
    if !status.path.is_empty() {
        println!("Path: {}", status.path);
    }
    if status.track_id != 0 && status.q_size > 0 {
        println!("Queue: track {} of {}", status.q_idx + 1, status.q_size);
    } else {
        println!("Queue: {} tracks", status.q_size);
    }
    if !status.version.is_empty() {
        println!("Version: {}", status.version);
    }
}

fn print_queue_tracks(tracks: &[Track], highlight: i64, color: bool) {
    println!("Queue ({} tracks):", tracks.len());
    println!(
        "{} {}  {} {} {}",
        pad_left("#", 3),
        pad_left("ID", 6),
        pad_right("Artist", 40),
        pad_right("Title", 40),
        pad_left("Dur", 8)
    );
    for (index, track) in tracks.iter().enumerate() {
        let is_current = highlight != 0 && track.id == highlight;
        let row = format!(
            "{} {}  {} {} {}",
            pad_left(&index.to_string(), 3),
            pad_left(&track.id.to_string(), 6),
            pad_right(&truncate_field(&track.artist, 40), 40),
            pad_right(&truncate_field(&track.title, 40), 40),
            pad_left(&format_time(track.duration), 8)
        );
        let marker = if is_current { ">" } else { " " };
        if is_current && color {
            println!("{marker} \u{1b}[32m{row}\u{1b}[0m");
        } else {
            println!("{marker} {row}");
        }
    }
}

fn print_queues(entries: &[crate::ipc::QueueEntry]) {
    println!("Queues ({}):", entries.len());
    println!(
        "{} {}  {} {} {} {}",
        pad_left("#", 3),
        pad_left("ID", 6),
        pad_right("Name", 40),
        pad_left("Tracks", 8),
        pad_left("Active", 6),
        pad_left("Temp", 4)
    );
    for (index, entry) in entries.iter().enumerate() {
        let name = if entry.name.is_empty() {
            "(unnamed)"
        } else {
            entry.name.as_str()
        };
        println!(
            "{} {}  {} {} {} {}",
            pad_left(&index.to_string(), 3),
            pad_left(&entry.id.to_string(), 6),
            pad_right(&truncate_field(name, 40), 40),
            pad_left(&entry.tracks.to_string(), 8),
            pad_left(if entry.active { "*" } else { "" }, 6),
            pad_left(if entry.temp { "yes" } else { "" }, 4)
        );
    }
}

pub fn print_volume(vol: f32, muted: bool) {
    if muted {
        println!("Volume: muted");
    } else {
        println!("Volume: {}%", (vol * 100.0_f32) as i32);
    }
}

fn print_track_info(track: &Track, play_count: i64, last_played: i64) {
    let field = |value: &str| {
        if value.is_empty() {
            "---".to_string()
        } else {
            value.to_string()
        }
    };
    let number = |value: i64| {
        if value == 0 {
            "---".to_string()
        } else {
            value.to_string()
        }
    };
    println!("Track [{}]", track.id);
    println!("  Path:         {}", track.path);
    println!("  Title:        {}", field(&track.title));
    println!("  Artist:       {}", field(&track.artist));
    println!("  Album:        {}", field(&track.album));
    println!("  Album Artist: {}", field(&track.album_artist));
    println!("  Genre:        {}", field(&track.genre));
    println!("  Year:         {}", number(i64::from(track.year)));
    println!("  Track:        {}", number(i64::from(track.track_num)));
    println!("  Disc:         {}", number(i64::from(track.disc_num)));
    println!("  Duration:     {}", format_time(track.duration));
    println!("  Sample Rate:  {} Hz", track.sample_rate);
    println!("  Channels:     {}", track.channels);
    if track.bitrate == 0 {
        println!("  Bitrate:      ---");
    } else {
        println!("  Bitrate:      {} kbps", track.bitrate / 1000);
    }
    println!("  Play Count:   {play_count}");
    if last_played > 0 {
        println!("  Last Played:  {}", format_epoch_ms(last_played));
    } else {
        println!("  Last Played:  (never)");
    }
}

/// UTC rendering of epoch milliseconds (the C++ formatter uses localtime;
/// timestamps are informational only).
fn format_epoch_ms(millis: i64) -> String {
    let seconds = millis.div_euclid(1000);
    let days = seconds.div_euclid(86_400);
    let seconds_of_day = seconds.rem_euclid(86_400);
    let (year, month, day) = civil_from_days(days);
    format!(
        "{year:04}-{month:02}-{day:02} {:02}:{:02}",
        seconds_of_day / 3600,
        (seconds_of_day % 3600) / 60
    )
}

/// Howard Hinnant's `civil_from_days` (proleptic Gregorian).
fn civil_from_days(days: i64) -> (i64, u32, u32) {
    let shifted = days + 719_468;
    let era = if shifted >= 0 {
        shifted
    } else {
        shifted - 146_096
    } / 146_097;
    let day_of_era = (shifted - era * 146_097) as u64;
    let year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let year = year_of_era as i64 + era * 400;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let month_prime = (5 * day_of_year + 2) / 153;
    let day = (day_of_year - (153 * month_prime + 2) / 5 + 1) as u32;
    let month = if month_prime < 10 {
        month_prime + 3
    } else {
        month_prime - 9
    } as u32;
    (if month <= 2 { year + 1 } else { year }, month, day)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn formats_times_like_the_cpp_formatter() {
        assert_eq!(format_time(-5.0), "00:00");
        assert_eq!(format_time(59.9), "00:59");
        assert_eq!(format_time(95.0), "01:35");
        assert_eq!(format_time(3723.0), "01:02:03");
    }

    #[test]
    fn truncates_with_ellipsis() {
        assert_eq!(truncate_field("", 40), "---");
        assert_eq!(truncate_field("short", 40), "short");
        assert_eq!(truncate_field(&"x".repeat(50), 10), "xxxxxxx...");
    }

    #[test]
    fn pads_by_byte_width() {
        assert_eq!(pad_left("7", 3), "  7");
        assert_eq!(pad_right("ab", 4), "ab  ");
    }

    #[test]
    fn epoch_conversion() {
        assert_eq!(format_epoch_ms(0), "1970-01-01 00:00");
        assert_eq!(format_epoch_ms(1_700_000_000_000), "2023-11-14 22:13");
    }
}
