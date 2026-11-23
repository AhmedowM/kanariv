//! Human renderer matching `cli/src/shell/output_formatter.cpp`.
//!
//! Everything renders into a `String` so batch handlers can fold a result
//! into their stdout blob; the `print_*` helpers are the single-shot
//! frontend path. String widths are measured in bytes, like `std::format` on
//! `std::string`, so table alignment matches the C++ output exactly for
//! byte-oriented text.

use serde_json::Value;

use crate::ipc::{LibraryStats, Payload, Playlist, QueueEntry, Reply, Status, TopEntity, Track};

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

/// `kanariv::println`: appends a newline between lines, never at the end.
fn pushln(out: &mut String, line: impl AsRef<str>) {
    if !out.is_empty() {
        out.push('\n');
    }
    out.push_str(line.as_ref());
}

/// Formatter state that the CLI attaches before printing: the `--columns`
/// allowlist of `library list` and the highlight needle of `library search`.
#[derive(Clone, Copy, Debug, Default)]
pub struct RenderOpts<'a> {
    pub columns: &'a str,
    pub needle: &'a str,
}

pub fn print_json(payload: &Payload) {
    print_json_value(&payload.json);
}

pub fn print_json_value(value: &Value) {
    match serde_json::to_string_pretty(value) {
        Ok(text) => println!("{text}"),
        Err(err) => eprintln!("Error: Corrupt {err}"),
    }
}

/// Renders a result in human form. `highlight` is the id to mark in a queue
/// listing (0 = none), matching the C++ formatter's `> ` marker.
pub fn print_payload(payload: &Payload, highlight: i64, color: bool) {
    print_payload_with(payload, highlight, color, &RenderOpts::default());
}

pub fn print_payload_with(payload: &Payload, highlight: i64, color: bool, opts: &RenderOpts) {
    println!("{}", render_payload_with(payload, highlight, color, opts));
}

#[must_use]
pub fn render_payload(payload: &Payload, highlight: i64, color: bool) -> String {
    render_payload_with(payload, highlight, color, &RenderOpts::default())
}

#[must_use]
pub fn render_payload_with(
    payload: &Payload,
    highlight: i64,
    color: bool,
    opts: &RenderOpts,
) -> String {
    let mut out = String::new();
    match &payload.reply {
        Reply::Status(status) => render_status(&mut out, status),
        Reply::QueueTracks(tracks) => render_queue_tracks(&mut out, tracks, highlight, color),
        Reply::Queues(entries) => render_queues(&mut out, entries),
        Reply::QueueCreated { id, name } => {
            pushln(&mut out, format!("Created queue {id} '{name}'"));
        }
        Reply::PlaylistLoaded { queue_id, .. } => {
            pushln(&mut out, format!("Loaded playlist into queue {queue_id}"));
        }
        Reply::VolumeInfo { vol, muted } => render_volume(&mut out, *vol, *muted),
        Reply::TrackInfo {
            track,
            play_count,
            last_played,
        } => render_track_info(&mut out, track, *play_count, *last_played),
        Reply::SingleTrack { track, .. } => render_track_detail(&mut out, track),
        Reply::ConfigValue(value) => {
            pushln(&mut out, format!("{} = {}", value.key, value.value));
        }
        Reply::ConfigValues(values) => {
            pushln(&mut out, format!("Config ({} entries):", values.len()));
            for value in values {
                pushln(&mut out, format!("{} = {}", value.key, value.value));
            }
        }
        Reply::Devices(devices) => render_devices(&mut out, devices),
        Reply::Playlists(playlists) => render_playlists(&mut out, playlists),
        Reply::PlaylistData { tracks, .. } => {
            pushln(&mut out, format!("Playlist ({} tracks):", tracks.len()));
            render_track_table(&mut out, tracks);
        }
        Reply::PlaylistCreated { id, name } => {
            pushln(&mut out, format!("Created playlist {id} '{name}'"));
        }
        Reply::PlaylistImportReport {
            name,
            tracks,
            matched,
            skipped,
            duplicates,
            ..
        } => {
            pushln(
                &mut out,
                format!(
                    "Imported playlist '{name}' ({} tracks, {matched} matched, {skipped} skipped, \
                     {duplicates} duplicates):",
                    tracks.len()
                ),
            );
            render_track_table(&mut out, tracks);
        }
        Reply::LibraryStats(stats) => render_library_stats(&mut out, stats),
        Reply::Tracks(tracks) => render_tracks(&mut out, tracks, opts.columns),
        Reply::SearchResults { tracks, .. } => {
            render_search_results(&mut out, tracks, opts.needle, color);
        }
        Reply::ScanReport { added } => {
            crate::batch::count_line_text(usize::try_from(*added).unwrap_or_default(), &mut out);
        }
        Reply::AnalyzeReport {
            tracks,
            analyzed,
            failed,
            skipped,
        } => {
            pushln(
                &mut out,
                format!("ReplayGain: {analyzed} analyzed, {failed} failed, {skipped} skipped of {tracks} tracks"),
            );
        }
        Reply::LibraryMaintenanceReport {
            tracks_checked,
            missing_tracks,
            duplicate_groups,
            duplicate_tracks,
        } => {
            render_maintenance(
                &mut out,
                *tracks_checked,
                missing_tracks,
                *duplicate_groups,
                *duplicate_tracks,
            );
        }
        Reply::Empty => pushln(&mut out, "OK"),
        _ => {
            out = serde_json::to_string_pretty(&payload.json)
                .unwrap_or_else(|err| format!("Error: Corrupt {err}"));
        }
    }
    out
}

fn render_devices(out: &mut String, devices: &[crate::ipc::DeviceInfo]) {
    pushln(out, format!("Devices ({}):", devices.len()));
    pushln(
        out,
        format!(
            "{}  {}  {}  Default",
            pad_left("#", 3),
            pad_right("ID", 20),
            pad_right("Name", 40)
        ),
    );
    for (index, device) in devices.iter().enumerate() {
        pushln(
            out,
            format!(
                "{}  {}  {}  {}",
                pad_left(&index.to_string(), 3),
                pad_right(&truncate_field(&device.id, 20), 20),
                pad_right(&truncate_field(&device.name, 40), 40),
                if device.is_default { "yes" } else { "no" }
            ),
        );
    }
}

fn render_status(out: &mut String, status: &Status) {
    pushln(out, format!("State: {}", status.state));
    pushln(
        out,
        format!(
            "Pos: {} / {}",
            format_time(status.pos),
            format_time(status.dur)
        ),
    );
    if status.muted {
        pushln(out, "Volume: muted");
    } else {
        pushln(out, format!("Volume: {}%", (status.vol * 100.0_f32) as i32));
    }
    pushln(
        out,
        format!(
            "Shuffle: {} Repeat: {}",
            if status.shuffle { "on" } else { "off" },
            status.repeat
        ),
    );
    if !status.artist.is_empty() && !status.title.is_empty() {
        pushln(
            out,
            format!(
                "Track: {} - {} [id: {}]",
                truncate_field(&status.artist, 40),
                truncate_field(&status.title, 40),
                status.track_id
            ),
        );
    } else if !status.artist.is_empty() || !status.title.is_empty() {
        pushln(
            out,
            format!(
                "Track: {} [id: {}]",
                truncate_field(&(status.artist.clone() + &status.title), 40),
                status.track_id
            ),
        );
    } else if status.track_id != 0 {
        pushln(out, format!("Track: [id: {}]", status.track_id));
    }
    if !status.path.is_empty() {
        pushln(out, format!("Path: {}", status.path));
    }
    if status.track_id != 0 && status.q_size > 0 {
        pushln(
            out,
            format!("Queue: track {} of {}", status.q_idx + 1, status.q_size),
        );
    } else {
        pushln(out, format!("Queue: {} tracks", status.q_size));
    }
    if !status.version.is_empty() {
        pushln(out, format!("Version: {}", status.version));
    }
}

fn render_queue_tracks(out: &mut String, tracks: &[Track], highlight: i64, color: bool) {
    pushln(out, format!("Queue ({} tracks):", tracks.len()));
    pushln(
        out,
        format!(
            "{} {}  {} {} {}",
            pad_left("#", 3),
            pad_left("ID", 6),
            pad_right("Artist", 40),
            pad_right("Title", 40),
            pad_left("Dur", 8)
        ),
    );
    for (index, track) in tracks.iter().enumerate() {
        let row = format!(
            "{} {}  {} {} {}",
            pad_left(&index.to_string(), 3),
            pad_left(&track.id.to_string(), 6),
            pad_right(&truncate_field(&track.artist, 40), 40),
            pad_right(&truncate_field(&track.title, 40), 40),
            pad_left(&format_time(track.duration), 8)
        );
        let is_current = highlight != 0 && track.id == highlight;
        if is_current && color {
            pushln(out, format!("> \u{1b}[32m{row}\u{1b}[0m"));
        } else if is_current {
            pushln(out, format!("> {row}"));
        } else {
            pushln(out, format!("  {row}"));
        }
    }
}

fn render_queues(out: &mut String, entries: &[QueueEntry]) {
    pushln(out, format!("Queues ({}):", entries.len()));
    pushln(
        out,
        format!(
            "{} {}  {} {} {} {}",
            pad_left("#", 3),
            pad_left("ID", 6),
            pad_right("Name", 40),
            pad_left("Tracks", 8),
            pad_left("Active", 6),
            pad_left("Temp", 4)
        ),
    );
    for (index, entry) in entries.iter().enumerate() {
        let name = if entry.name.is_empty() {
            "(unnamed)"
        } else {
            entry.name.as_str()
        };
        pushln(
            out,
            format!(
                "{} {}  {} {} {} {}",
                pad_left(&index.to_string(), 3),
                pad_left(&entry.id.to_string(), 6),
                pad_right(&truncate_field(name, 40), 40),
                pad_left(&entry.tracks.to_string(), 8),
                pad_left(if entry.active { "*" } else { "" }, 6),
                pad_left(if entry.temp { "yes" } else { "" }, 4)
            ),
        );
    }
}

fn render_playlists(out: &mut String, playlists: &[Playlist]) {
    pushln(out, format!("Playlists ({}):", playlists.len()));
    pushln(out, format!("{:>3} {:>6}  {:<40}", "#", "ID", "Name"));
    for (index, playlist) in playlists.iter().enumerate() {
        pushln(
            out,
            format!(
                "{index:>3} {:>6}  {}",
                playlist.id,
                pad_right(&truncate_field(&playlist.name, 40), 40)
            ),
        );
    }
}

/// Shared table for `PlaylistData` and `PlaylistImportReport`.
fn render_track_table(out: &mut String, tracks: &[Track]) {
    pushln(
        out,
        format!(
            "{:>3} {:>6}  {:<40} {:<40} {:>8}",
            "#", "ID", "Artist", "Title", "Dur"
        ),
    );
    for (index, track) in tracks.iter().enumerate() {
        pushln(
            out,
            format!(
                "{index:>3} {:>6}  {} {} {:>8}",
                track.id,
                pad_right(&truncate_field(&track.artist, 40), 40),
                pad_right(&truncate_field(&track.title, 40), 40),
                format_time(track.duration)
            ),
        );
    }
}

/// `OutputFormatter::setColumns`: comma-separated allowlist, spaces and tabs
/// trimmed off each name, blanks dropped.
#[must_use]
pub fn parse_columns(text: &str) -> Vec<String> {
    text.split(',')
        .map(|column| column.trim_matches(|char| char == ' ' || char == '\t'))
        .filter(|column| !column.is_empty())
        .map(ToString::to_string)
        .collect()
}

/// `OutputFormatter`'s `highlightAll`: wraps every case-insensitive byte-wise
/// occurrence of `needle` in ANSI green.
#[must_use]
pub fn highlight_all(text: &str, needle: &str, color: bool) -> String {
    if !color || needle.is_empty() {
        return text.to_string();
    }
    let bytes = text.as_bytes();
    let pattern = needle.as_bytes();
    let mut out: Vec<u8> = Vec::with_capacity(bytes.len());
    let mut pos = 0_usize;
    while pos < bytes.len() {
        let mut hit = None;
        let mut index = pos;
        while index + pattern.len() <= bytes.len() {
            if bytes[index..index + pattern.len()]
                .iter()
                .zip(pattern)
                .all(|(found, want)| found.eq_ignore_ascii_case(want))
            {
                hit = Some(index);
                break;
            }
            index += 1;
        }
        let Some(hit) = hit else {
            out.extend_from_slice(&bytes[pos..]);
            break;
        };
        out.extend_from_slice(&bytes[pos..hit]);
        out.extend_from_slice(b"\x1b[32m");
        out.extend_from_slice(&bytes[hit..hit + pattern.len()]);
        out.extend_from_slice(b"\x1b[0m");
        pos = hit + pattern.len();
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// `ipc::Tracks`: the default artist/title table, or the `--columns` subset.
fn render_tracks(out: &mut String, tracks: &[Track], columns: &str) {
    pushln(out, format!("Tracks ({}):", tracks.len()));
    let selected = parse_columns(columns);
    if selected.is_empty() {
        pushln(
            out,
            format!(
                "{} {}  {} {} {}",
                pad_left("#", 3),
                pad_left("ID", 6),
                pad_right("Artist", 40),
                pad_right("Title", 40),
                pad_left("Dur", 8)
            ),
        );
        for (index, track) in tracks.iter().enumerate() {
            pushln(
                out,
                format!(
                    "{} {}  {} {} {}",
                    pad_left(&index.to_string(), 3),
                    pad_left(&track.id.to_string(), 6),
                    pad_right(&truncate_field(&track.artist, 40), 40),
                    pad_right(&truncate_field(&track.title, 40), 40),
                    pad_left(&format_time(track.duration), 8)
                ),
            );
        }
        return;
    }
    pushln(out, track_columns_header(&selected));
    for (index, track) in tracks.iter().enumerate() {
        pushln(out, track_columns_row(track, index, &selected));
    }
}

fn track_columns_header(selected: &[String]) -> String {
    let mut line = pad_left("#", 3);
    let mut append = |name: &str, field: &str| {
        if selected.iter().any(|column| column == name) {
            line.push(' ');
            line.push_str(field);
        }
    };
    append("id", &pad_left("ID", 6));
    append("title", &pad_right("Title", 40));
    append("artist", &pad_right("Artist", 40));
    append("album", &pad_right("Album", 40));
    append("genre", &pad_right("Genre", 20));
    append("year", &pad_left("Year", 6));
    append("rating", &pad_left("Rating", 8));
    append("duration", &pad_left("Dur", 8));
    append("path", "Path");
    line
}

fn track_columns_row(track: &Track, index: usize, selected: &[String]) -> String {
    let wants = |name: &str| selected.iter().any(|column| column == name);
    let mut line = pad_left(&index.to_string(), 3);
    if wants("id") {
        line.push(' ');
        line.push_str(&pad_left(&track.id.to_string(), 6));
    }
    if wants("title") {
        line.push(' ');
        line.push_str(&pad_right(&truncate_field(&track.title, 40), 40));
    }
    if wants("artist") {
        line.push(' ');
        line.push_str(&pad_right(&truncate_field(&track.artist, 40), 40));
    }
    if wants("album") {
        line.push(' ');
        line.push_str(&pad_right(&truncate_field(&track.album, 40), 40));
    }
    if wants("genre") {
        line.push(' ');
        line.push_str(&pad_right(&truncate_field(&track.genre, 20), 20));
    }
    if wants("year") {
        // `{:6}` on a std::string: `std::format` left-aligns strings.
        line.push(' ');
        let year = if track.year == 0 {
            "---".to_string()
        } else {
            track.year.to_string()
        };
        line.push_str(&pad_right(&year, 6));
    }
    if wants("rating") {
        line.push(' ');
        line.push_str(&pad_left(&track.rating.to_string(), 8));
    }
    if wants("duration") {
        line.push(' ');
        line.push_str(&pad_left(&format_time(track.duration), 8));
    }
    if wants("path") {
        line.push(' ');
        line.push_str(&track.path);
    }
    line
}

/// `ipc::SearchResults`: fixed-width table with the query highlighted.
fn render_search_results(out: &mut String, tracks: &[Track], needle: &str, color: bool) {
    if tracks.len() == 1 {
        pushln(out, "1 track found:");
    } else {
        pushln(out, format!("{} tracks found:", tracks.len()));
    }
    pushln(
        out,
        format!(
            "{} {}  {} {} {} {} {}",
            pad_left("#", 3),
            pad_left("ID", 6),
            pad_right("Title", 30),
            pad_right("Artist", 25),
            pad_right("Album", 20),
            pad_right("Genre", 12),
            pad_right("File", 25)
        ),
    );
    let cell = |value: &str, width: usize| {
        pad_right(
            &highlight_all(&truncate_field(value, width), needle, color),
            width,
        )
    };
    for (index, track) in tracks.iter().enumerate() {
        let file = crate::paths::filename_of(&track.path);
        pushln(
            out,
            format!(
                "{} {}  {} {} {} {} {}",
                pad_left(&index.to_string(), 3),
                pad_left(&track.id.to_string(), 6),
                cell(&track.title, 30),
                cell(&track.artist, 25),
                cell(&track.album, 20),
                cell(&track.genre, 12),
                cell(&file, 25)
            ),
        );
    }
}

/// `types::LibraryStats`: one totals line, or the detailed view once any
/// top-N report is engaged (presence, not emptiness, is the trigger).
fn render_library_stats(out: &mut String, stats: &LibraryStats) {
    let detailed = stats.top_tracks.is_some()
        || stats.top_albums.is_some()
        || stats.top_genres.is_some()
        || stats.top_artists.is_some();
    if !detailed {
        pushln(
            out,
            format!(
                "Tracks: {} Queues: {} Playlists: {}",
                stats.tracks, stats.queues, stats.playlists
            ),
        );
        return;
    }
    pushln(out, "Library Stats (Detailed):");
    pushln(out, format!("  Tracks:      {}", stats.tracks));
    pushln(out, format!("  Queues:      {}", stats.queues));
    pushln(out, format!("  Playlists:   {}", stats.playlists));
    pushln(
        out,
        format!(
            "  Total Duration:  {}",
            format_time(stats.total_duration_ms as f64 / 1000.0)
        ),
    );
    pushln(
        out,
        format!(
            "  Total Play Time: {}",
            format_time(stats.total_play_time_ms as f64 / 1000.0)
        ),
    );
    if let Some(tracks) = stats
        .top_tracks
        .as_ref()
        .filter(|tracks| !tracks.is_empty())
    {
        pushln(out, "\n  Most Played Tracks:");
        pushln(
            out,
            format!(
                "  {} {}  {} {} {} {}",
                pad_left("#", 3),
                pad_left("ID", 6),
                pad_right("Artist", 40),
                pad_right("Title", 40),
                pad_left("Dur", 8),
                pad_left("Plays", 10)
            ),
        );
        for (index, track) in tracks.iter().enumerate() {
            pushln(
                out,
                format!(
                    "  {} {}  {} {} {} {}",
                    pad_left(&(index + 1).to_string(), 3),
                    pad_left(&track.id.to_string(), 6),
                    pad_right(&truncate_field(&track.artist, 40), 40),
                    pad_right(&truncate_field(&track.title, 40), 40),
                    pad_left(&format_time(track.duration), 8),
                    pad_left(&track.play_count.to_string(), 10)
                ),
            );
        }
    }
    render_top_entities(
        out,
        "\n  Top Albums by Plays:",
        "Album",
        40,
        stats.top_albums.as_deref(),
    );
    render_top_entities(
        out,
        "\n  Top Genres by Plays:",
        "Genre",
        20,
        stats.top_genres.as_deref(),
    );
    render_top_entities(
        out,
        "\n  Top Artists by Plays:",
        "Artist",
        40,
        stats.top_artists.as_deref(),
    );
}

fn render_top_entities(
    out: &mut String,
    title: &str,
    label: &str,
    width: usize,
    entries: Option<&[TopEntity]>,
) {
    let Some(entries) = entries.filter(|entries| !entries.is_empty()) else {
        return;
    };
    pushln(out, title);
    pushln(
        out,
        format!(
            "  {} {} {} {}",
            pad_left("#", 3),
            pad_right(label, width),
            pad_left("Tracks", 10),
            pad_left("Plays", 10)
        ),
    );
    for (index, entry) in entries.iter().enumerate() {
        pushln(
            out,
            format!(
                "  {} {} {} {}",
                pad_left(&(index + 1).to_string(), 3),
                pad_right(&truncate_field(&entry.name, width), width),
                pad_left(&entry.track_count.to_string(), 10),
                pad_left(&entry.total_plays.to_string(), 10)
            ),
        );
    }
}

/// `ipc::LibraryMaintenanceReport`.
fn render_maintenance(
    out: &mut String,
    tracks_checked: u64,
    missing: &[Track],
    duplicate_groups: u64,
    duplicate_tracks: u64,
) {
    pushln(
        out,
        format!("Library maintenance: {tracks_checked} tracks checked"),
    );
    if missing.is_empty() && duplicate_groups == 0 {
        pushln(out, "  No missing files or duplicates found.");
        return;
    }
    if !missing.is_empty() {
        pushln(out, format!("  Missing files ({}):", missing.len()));
        pushln(
            out,
            format!(
                "{} {}  {} {}",
                pad_left("#", 3),
                pad_left("ID", 6),
                pad_right("Artist", 40),
                pad_right("Title", 40)
            ),
        );
        for (index, track) in missing.iter().enumerate() {
            pushln(
                out,
                format!(
                    "{} {}  {} {}",
                    pad_left(&index.to_string(), 3),
                    pad_left(&track.id.to_string(), 6),
                    pad_right(&truncate_field(&track.artist, 40), 40),
                    pad_right(&truncate_field(&track.title, 40), 40)
                ),
            );
        }
    }
    if duplicate_groups > 0 {
        pushln(
            out,
            format!("  Duplicate groups: {duplicate_groups} (redundant rows: {duplicate_tracks})"),
        );
    }
}

fn render_volume(out: &mut String, vol: f32, muted: bool) {
    if muted {
        pushln(out, "Volume: muted");
    } else {
        pushln(out, format!("Volume: {}%", (vol * 100.0_f32) as i32));
    }
}
fn render_track_info(out: &mut String, track: &Track, play_count: i64, last_played: i64) {
    render_track_detail(out, track);
    pushln(out, format!("  Play Count:   {play_count}"));
    if last_played > 0 {
        pushln(
            out,
            format!("  Last Played:  {}", format_epoch_ms(last_played)),
        );
    } else {
        pushln(out, "  Last Played:  (never)");
    }
}

/// `OutputFormatter::printTrackDetail`: the block `tag get` prints on its own,
/// and the first half of the `TrackInfo` block.
fn render_track_detail(out: &mut String, track: &Track) {
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
    pushln(out, format!("Track [{}]", track.id));
    pushln(out, format!("  Path:         {}", track.path));
    pushln(out, format!("  Title:        {}", field(&track.title)));
    pushln(out, format!("  Artist:       {}", field(&track.artist)));
    pushln(out, format!("  Album:        {}", field(&track.album)));
    pushln(
        out,
        format!("  Album Artist: {}", field(&track.album_artist)),
    );
    pushln(out, format!("  Genre:        {}", field(&track.genre)));
    pushln(
        out,
        format!("  Year:         {}", number(i64::from(track.year))),
    );
    pushln(
        out,
        format!("  Track:        {}", number(i64::from(track.track_num))),
    );
    pushln(
        out,
        format!("  Disc:         {}", number(i64::from(track.disc_num))),
    );
    pushln(
        out,
        format!("  Duration:     {}", format_time(track.duration)),
    );
    pushln(out, format!("  Sample Rate:  {} Hz", track.sample_rate));
    pushln(out, format!("  Channels:     {}", track.channels));
    if track.bitrate == 0 {
        pushln(out, "  Bitrate:      ---");
    } else {
        pushln(
            out,
            format!("  Bitrate:      {} kbps", track.bitrate / 1000),
        );
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

    fn payload(reply: Reply) -> Payload {
        Payload {
            json: Value::Null,
            reply,
        }
    }

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

    #[test]
    fn playlists_table_matches_the_cpp_widths() {
        let text = render_payload(
            &payload(Reply::Playlists(vec![Playlist {
                id: 3,
                name: "mix".to_string(),
                ..Playlist::default()
            }])),
            0,
            false,
        );
        let lines: Vec<&str> = text.lines().collect();
        assert_eq!(lines[0], "Playlists (1):");
        assert!(lines[1].starts_with("  #     ID  Name"));
        assert!(lines[2].starts_with("  0      3  mix"));
        assert_eq!(lines[1].len(), lines[2].len());
    }

    #[test]
    fn playlist_data_table_has_the_duration_column() {
        let track = Track {
            id: 7,
            artist: "Artist".to_string(),
            title: "Title".to_string(),
            duration: 95.0,
            ..Track::default()
        };
        let text = render_payload(
            &payload(Reply::PlaylistData {
                tracks: vec![track],
                format: String::new(),
            }),
            0,
            false,
        );
        let lines: Vec<&str> = text.lines().collect();
        assert_eq!(lines[0], "Playlist (1 tracks):");
        assert!(lines[1].starts_with("  #     ID  Artist"));
        assert!(lines[1].ends_with("     Dur"));
        assert!(lines[2].starts_with("  0      7  Artist"));
        assert!(lines[2].ends_with("    01:35"));
    }

    #[test]
    fn import_report_prefix_matches_cpp() {
        let text = render_payload(
            &payload(Reply::PlaylistImportReport {
                pid: 4,
                name: "imported".to_string(),
                tracks: Vec::new(),
                matched: 2,
                skipped: 1,
                duplicates: 0,
            }),
            0,
            false,
        );
        let lines: Vec<&str> = text.lines().collect();
        assert_eq!(
            lines[0],
            "Imported playlist 'imported' (0 tracks, 2 matched, 1 skipped, 0 duplicates):"
        );
    }

    #[test]
    fn single_track_renders_the_detail_block_without_play_stats() {
        let text = render_payload(
            &payload(Reply::SingleTrack {
                track: sample_track(),
                cover_mime: None,
                cover_data: None,
            }),
            0,
            false,
        );
        assert_eq!(
            text,
            "Track [7]\n  \
             Path:         \n  \
             Title:        Title\n  \
             Artist:       Artist\n  \
             Album:        ---\n  \
             Album Artist: ---\n  \
             Genre:        ---\n  \
             Year:         ---\n  \
             Track:        ---\n  \
             Disc:         ---\n  \
             Duration:     01:35\n  \
             Sample Rate:  0 Hz\n  \
             Channels:     0\n  \
             Bitrate:      ---"
        );
    }

    #[test]
    fn scan_and_analyze_reports_use_the_cpp_wording() {
        let one = render_payload(&payload(Reply::ScanReport { added: 1 }), 0, false);
        assert_eq!(one, "1 track added");
        let many = render_payload(&payload(Reply::ScanReport { added: 0 }), 0, false);
        assert_eq!(many, "0 tracks added");
        let analyze = render_payload(
            &payload(Reply::AnalyzeReport {
                tracks: 12,
                analyzed: 9,
                failed: 1,
                skipped: 2,
            }),
            0,
            false,
        );
        assert_eq!(
            analyze,
            "ReplayGain: 9 analyzed, 1 failed, 2 skipped of 12 tracks"
        );
    }

    #[test]
    fn tracks_default_table_matches_the_cpp_widths() {
        let text = render_payload(&payload(Reply::Tracks(vec![sample_track()])), 0, false);
        let lines: Vec<&str> = text.lines().collect();
        assert_eq!(lines[0], "Tracks (1):");
        assert_eq!(
            lines[1],
            format!("  #     ID  {:<40} {:<40}      Dur", "Artist", "Title")
        );
        assert_eq!(
            lines[2],
            format!("  0      7  {:<40} {:<40}    01:35", "Artist", "Title")
        );
    }

    #[test]
    fn tracks_columns_table_follows_the_allowlist() {
        let opts = RenderOpts {
            columns: " title , year,rating,bogus,path",
            needle: "",
        };
        let track = Track {
            title: "Title".to_string(),
            path: "D:/Music/one.wav".to_string(),
            ..Track::default()
        };
        let text = render_payload_with(&payload(Reply::Tracks(vec![track])), 0, false, &opts);
        let lines: Vec<&str> = text.lines().collect();
        assert_eq!(
            lines[1],
            format!("  # {:<40} {:>6} {:>8} Path", "Title", "Year", "Rating")
        );
        // Year carries a string (`---`), so `std::format` left-aligns it while
        // the numeric rating stays right-aligned.
        assert_eq!(
            lines[2],
            format!("  0 {:<40} {:<6} {:>8} D:/Music/one.wav", "Title", "---", 0)
        );
    }

    #[test]
    fn column_allowlist_parsing_trims_and_drops_blanks() {
        assert!(parse_columns("").is_empty());
        assert_eq!(parse_columns("a,, b ,\tc "), ["a", "b", "c"]);
    }

    #[test]
    fn search_table_highlights_only_on_a_terminal() {
        let track = Track {
            id: 3,
            title: "Rock Anthem".to_string(),
            artist: "The Rocks".to_string(),
            path: "D:/Music/rock/anthem.wav".to_string(),
            ..Track::default()
        };
        let opts = RenderOpts {
            columns: "",
            needle: "rock",
        };
        let plain = render_payload_with(
            &payload(Reply::SearchResults {
                query: "rock".to_string(),
                tracks: vec![track.clone()],
            }),
            0,
            false,
            &opts,
        );
        let lines: Vec<&str> = plain.lines().collect();
        assert_eq!(lines[0], "1 track found:");
        assert_eq!(
            lines[1],
            format!(
                "  #     ID  {:<30} {:<25} {:<20} {:<12} {:<25}",
                "Title", "Artist", "Album", "Genre", "File"
            )
        );
        assert_eq!(
            lines[2],
            format!(
                "  0      3  {:<30} {:<25} {:<20} {:<12} {:<25}",
                "Rock Anthem", "The Rocks", "---", "---", "anthem.wav"
            )
        );
        assert!(!plain.contains('\u{1b}'));

        let colored = render_payload_with(
            &payload(Reply::SearchResults {
                query: "rock".to_string(),
                tracks: vec![track],
            }),
            0,
            true,
            &opts,
        );
        assert!(colored.contains("\u{1b}[32mRock\u{1b}[0m Anthem"));
    }

    #[test]
    fn highlight_wraps_every_case_insensitive_occurrence() {
        assert_eq!(
            highlight_all("aBcAbc", "abc", true),
            "\u{1b}[32maBc\u{1b}[0m\u{1b}[32mAbc\u{1b}[0m"
        );
        assert_eq!(highlight_all("abc", "", true), "abc");
        assert_eq!(highlight_all("abc", "abc", false), "abc");
        assert_eq!(highlight_all("zz", "abc", true), "zz");
    }

    #[test]
    fn library_stats_totals_need_no_top_n_request() {
        let text = render_payload(
            &payload(Reply::LibraryStats(LibraryStats::default())),
            0,
            false,
        );
        assert_eq!(text, "Tracks: 0 Queues: 0 Playlists: 0");
    }

    #[test]
    fn library_stats_detailed_view_is_engaged_by_an_empty_top_n() {
        let stats = LibraryStats {
            tracks: 4,
            queues: 2,
            playlists: 1,
            total_duration_ms: 95_000,
            total_play_time_ms: 60_000,
            top_tracks: Some(Vec::new()),
            top_albums: Some(vec![TopEntity {
                name: "Album".to_string(),
                track_count: 2,
                total_plays: 9,
            }]),
            top_genres: Some(Vec::new()),
            top_artists: None,
        };
        let text = render_payload(&payload(Reply::LibraryStats(stats)), 0, false);
        let lines: Vec<&str> = text.lines().collect();
        assert_eq!(lines[0], "Library Stats (Detailed):");
        assert_eq!(lines[1], "  Tracks:      4");
        assert_eq!(lines[2], "  Queues:      2");
        assert_eq!(lines[3], "  Playlists:   1");
        assert_eq!(lines[4], "  Total Duration:  01:35");
        assert_eq!(lines[5], "  Total Play Time: 01:00");
        // An engaged-but-empty report emits nothing, so the blank line before
        // the albums section separates it from the totals.
        assert_eq!(lines[6], "");
        assert_eq!(lines[7], "  Top Albums by Plays:");
        assert_eq!(
            lines[8],
            format!("    # {:<40} {:>10} {:>10}", "Album", "Tracks", "Plays")
        );
        assert_eq!(
            lines[9],
            format!("    1 {:<40} {:>10} {:>10}", "Album", 2, 9)
        );
    }

    #[test]
    fn maintenance_report_sections() {
        let clean = render_payload(
            &payload(Reply::LibraryMaintenanceReport {
                tracks_checked: 5,
                missing_tracks: Vec::new(),
                duplicate_groups: 0,
                duplicate_tracks: 0,
            }),
            0,
            false,
        );
        assert_eq!(
            clean,
            "Library maintenance: 5 tracks checked\n  No missing files or duplicates found."
        );

        let dirty = render_payload(
            &payload(Reply::LibraryMaintenanceReport {
                tracks_checked: 5,
                missing_tracks: vec![sample_track()],
                duplicate_groups: 1,
                duplicate_tracks: 2,
            }),
            0,
            false,
        );
        let lines: Vec<&str> = dirty.lines().collect();
        assert_eq!(lines[1], "  Missing files (1):");
        assert!(lines[3].starts_with("  0      7  Artist"));
        assert_eq!(lines[4], "  Duplicate groups: 1 (redundant rows: 2)");
    }

    #[test]
    fn library_stats_prints_every_top_n_table() {
        // A fresh library has no plays, so the live parity harness cannot reach
        // these table bodies; pin their bytes instead.
        let stats = LibraryStats {
            tracks: 4,
            queues: 2,
            playlists: 1,
            total_duration_ms: 95_000,
            total_play_time_ms: 60_000,
            top_tracks: Some(vec![sample_track()]),
            top_albums: Some(vec![top_entity("Album", 2, 9)]),
            top_genres: Some(vec![top_entity(&"g".repeat(25), 3, 8)]),
            top_artists: Some(vec![top_entity("Artist", 4, 7)]),
        };
        let text = render_payload(&payload(Reply::LibraryStats(stats)), 0, false);
        let lines: Vec<&str> = text.lines().collect();
        assert_eq!(lines[6], "");
        assert_eq!(lines[7], "  Most Played Tracks:");
        assert_eq!(
            lines[8],
            format!(
                "    #     ID  {:<40} {:<40}      Dur      Plays",
                "Artist", "Title"
            )
        );
        assert_eq!(
            lines[9],
            format!(
                "    1      7  {:<40} {:<40}    01:35          0",
                "Artist", "Title"
            )
        );
        assert_eq!(lines[10], "");
        assert_eq!(lines[11], "  Top Albums by Plays:");
        assert_eq!(
            lines[13],
            format!("    1 {:<40} {:>10} {:>10}", "Album", 2, 9)
        );
        assert_eq!(lines[14], "");
        assert_eq!(lines[15], "  Top Genres by Plays:");
        // The genre column is 20 wide, so a long name is truncated.
        assert_eq!(
            lines[17],
            format!(
                "    1 {:<20} {:>10} {:>10}",
                truncate_field(&"g".repeat(25), 20),
                3,
                8
            )
        );
        assert_eq!(lines[18], "");
        assert_eq!(lines[19], "  Top Artists by Plays:");
        assert_eq!(
            lines[21],
            format!("    1 {:<40} {:>10} {:>10}", "Artist", 4, 7)
        );
    }

    fn sample_track() -> Track {
        Track {
            id: 7,
            artist: "Artist".to_string(),
            title: "Title".to_string(),
            duration: 95.0,
            ..Track::default()
        }
    }

    fn top_entity(name: &str, track_count: u64, total_plays: i64) -> TopEntity {
        TopEntity {
            name: name.to_string(),
            track_count,
            total_plays,
        }
    }
}
