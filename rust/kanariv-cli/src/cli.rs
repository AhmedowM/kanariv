//! Command-line surface (clap).
//!
//! Parse failures keep clap's exit 2, which is what the C++ shell normalizes
//! CLI11's own codes to (`detail::exitUsage`). Help/version displays exit 0.

use std::path::PathBuf;

use clap::{Args, Parser, Subcommand};

#[derive(Debug, Parser)]
#[command(
    name = "kanariv",
    bin_name = "kanariv",
    about = "kanariv - terminal player (Rust experimental CLI)",
    disable_version_flag = true,
    subcommand_help_heading = "SUBCOMMANDS"
)]
pub struct Cli {
    /// Config file
    #[arg(long, global = true, value_name = "PATH")]
    pub config: Option<PathBuf>,
    /// trace|debug|info|warn|error
    #[arg(long = "log-level", global = true, value_name = "LEVEL")]
    pub log_level: Option<String>,
    /// Audio output device
    #[arg(long, global = true, value_name = "DEVICE")]
    pub device: Option<String>,
    /// Print the version and exit
    #[arg(long = "version", action = clap::ArgAction::SetTrue, global = true)]
    pub version: bool,
    #[command(subcommand)]
    pub command: Option<Command>,
}

#[derive(Debug, Subcommand)]
pub enum Command {
    /// Show version and build details
    Version {
        #[arg(long)]
        json: bool,
    },
    /// Start daemon
    Start {
        /// Run in foreground
        #[arg(long)]
        foreground: bool,
    },
    /// Stop daemon
    Shutdown,
    /// Play current queue
    Play {
        #[arg(long)]
        json: bool,
    },
    /// Pause playback
    Pause {
        #[arg(long)]
        json: bool,
    },
    /// Resume playback
    Resume {
        #[arg(long)]
        json: bool,
    },
    /// Restart current track
    Restart {
        #[arg(long)]
        json: bool,
    },
    /// Stop playback
    Stop {
        #[arg(long)]
        json: bool,
    },
    /// Next track
    Next {
        #[arg(long)]
        json: bool,
    },
    /// Prev track
    Prev {
        #[arg(long)]
        json: bool,
    },
    /// Seek to position
    Seek {
        /// mm:ss or seconds or +N/-N
        #[arg(allow_negative_numbers = true)]
        time: String,
        #[arg(long)]
        json: bool,
    },
    /// Show status
    Status(StatusArgs),
    /// Get/set volume
    Volume {
        /// 0-100|+N|-N|mute|unmute
        #[arg(allow_negative_numbers = true)]
        level: Option<String>,
        #[arg(long)]
        json: bool,
    },
    /// Queue operations
    Queue {
        #[command(subcommand)]
        command: Option<QueueCommand>,
    },
    /// Show current track info
    Info {
        #[arg(long)]
        json: bool,
    },
    /// Config operations
    Config {
        #[command(subcommand)]
        command: Option<ConfigCommand>,
    },
    /// Audio device operations
    Device {
        #[command(subcommand)]
        command: Option<DeviceCommand>,
    },
    /// Playlist operations
    Playlist {
        #[command(subcommand)]
        command: Option<PlaylistCommand>,
    },
    /// Library operations
    Library {
        #[command(subcommand)]
        command: Option<LibraryCommand>,
    },
}

#[derive(Debug, Args)]
pub struct StatusArgs {
    #[arg(long)]
    pub json: bool,
    /// Continuous polling
    #[arg(long)]
    pub watch: bool,
    /// Alias for --watch
    #[arg(long)]
    pub follow: bool,
    /// Poll interval in ms
    #[arg(long, default_value_t = 1000)]
    pub interval: i64,
}

#[derive(Debug, Subcommand)]
pub enum QueueCommand {
    /// List all queues
    List {
        #[arg(long)]
        json: bool,
    },
    /// List all queues (alias)
    Queues {
        #[arg(long)]
        json: bool,
    },
    /// List tracks in active queue
    Tracks {
        /// Track order: added|playback
        #[arg(long, default_value = "playback", value_parser = ["added", "playback"])]
        order: String,
        #[arg(long)]
        json: bool,
    },
    /// Switch active queue
    Switch {
        /// Queue id
        qid: i64,
        #[arg(long)]
        json: bool,
    },
    /// Create a new queue
    Create {
        /// Queue name
        name: String,
        #[arg(long)]
        json: bool,
    },
    /// Delete a queue
    Delete {
        /// Queue id
        qid: i64,
        #[arg(long)]
        json: bool,
    },
    /// Clear queue
    Clear {
        #[arg(long)]
        json: bool,
    },
    /// Set shuffle
    Shuffle {
        /// on|off
        mode: Option<String>,
        #[arg(long)]
        json: bool,
    },
    /// Set repeat
    Repeat {
        /// off|one|all
        mode: Option<String>,
        #[arg(long)]
        json: bool,
    },
}

#[derive(Debug, Subcommand)]
pub enum ConfigCommand {
    /// Get config value
    Get {
        /// Key
        key: String,
        #[arg(long)]
        json: bool,
    },
    /// Set config value
    Set {
        /// Key
        key: String,
        /// Value
        value: String,
    },
    /// List config
    List {
        #[arg(long)]
        json: bool,
    },
    /// Export config
    Export {
        /// Path
        path: String,
        #[arg(long)]
        json: bool,
    },
    /// Import config
    Import {
        /// Path
        path: String,
        #[arg(long)]
        json: bool,
    },
    /// Reset config to defaults
    Reset {
        /// Key to reset (omit to reset all)
        key: Option<String>,
        #[arg(long)]
        json: bool,
    },
}

#[derive(Debug, Subcommand)]
pub enum DeviceCommand {
    /// List audio output devices
    List {
        #[arg(long)]
        json: bool,
    },
    /// Set default audio device
    Set {
        /// Device index, name, or stable id
        id: String,
        #[arg(long)]
        json: bool,
    },
    /// Check audio device is available
    Test {
        /// Device index, name, or stable id (default: current config)
        #[arg(long)]
        id: Option<String>,
        /// Play an audible test tone
        #[arg(long)]
        tone: bool,
        #[arg(long)]
        json: bool,
    },
}

#[derive(Debug, Subcommand)]
pub enum PlaylistCommand {
    /// List playlists
    List {
        #[arg(long)]
        json: bool,
    },
    /// Tracks in playlist
    Tracks {
        /// Playlist id
        pid: i64,
        #[arg(long)]
        json: bool,
    },
    /// Create an empty playlist
    Create {
        /// Playlist name
        name: String,
        /// Smart-playlist filter expression
        #[arg(long)]
        smart: Option<String>,
        #[arg(long)]
        json: bool,
    },
    /// Add tracks to playlist
    Add {
        /// Playlist id
        pid: i64,
        /// Library track id (repeatable)
        #[arg(long = "id", value_name = "ID")]
        ids: Vec<String>,
        /// File, folder, or glob (repeatable)
        #[arg(value_name = "PATH")]
        paths: Vec<String>,
        /// Recurse into folders
        #[arg(long)]
        recursive: bool,
        #[arg(long)]
        json: bool,
    },
    /// Load playlist into queue
    Load {
        /// Playlist id
        pid: i64,
        /// Play after load
        #[arg(long)]
        play: bool,
        /// Replace active queue
        #[arg(long)]
        replace: bool,
        #[arg(long)]
        json: bool,
    },
    /// Save queue as playlist
    Save {
        /// Playlist name
        name: String,
        /// Queue id
        #[arg(long)]
        queue: Option<i64>,
        #[arg(long)]
        json: bool,
    },
    /// Delete playlist
    Delete {
        /// Playlist id
        pid: i64,
        #[arg(long)]
        json: bool,
    },
    /// Rename playlist
    Rename {
        /// Playlist id
        pid: i64,
        /// New name
        name: String,
        #[arg(long)]
        json: bool,
    },
    /// Export playlist to file
    Export {
        /// Playlist id
        pid: i64,
        /// Output file path
        path: String,
        /// Format: m3u|pls|json
        #[arg(long, default_value = "m3u", value_parser = ["m3u", "pls", "json"])]
        format: String,
    },
    /// Import playlist from file
    Import {
        /// Input file path
        path: String,
        /// Playlist name (default: filename)
        #[arg(long)]
        name: Option<String>,
        #[arg(long)]
        json: bool,
    },
}

#[derive(Debug, Subcommand)]
pub enum LibraryCommand {
    /// Scan library
    Scan {
        /// Scan path (default: library music dir)
        #[arg(long, value_name = "PATH")]
        path: Option<String>,
        /// Full-file BLAKE3 (slower)
        #[arg(long)]
        full_hash: bool,
        #[arg(long)]
        json: bool,
    },
    #[command(about = "Measure ReplayGain 2.0 gains")]
    Analyze {
        /// Re-measure tracks that already carry gains
        #[arg(long)]
        reanalyze: bool,
        /// Library id (0 = all tracks)
        #[arg(long, default_value_t = 0)]
        library: i64,
        #[arg(long)]
        json: bool,
    },
    /// Search library
    Search {
        /// FTS query
        query: String,
        /// Limit
        #[arg(long, default_value_t = 50)]
        limit: i32,
        #[arg(long)]
        json: bool,
    },
    /// Library stats
    Stats {
        /// Top-N most played tracks (0 = totals only)
        #[arg(long, default_value_t = -1, allow_hyphen_values = true)]
        top_tracks: i32,
        /// Top-N albums by total plays (0 = not requested)
        #[arg(long, default_value_t = 0, allow_hyphen_values = true)]
        top_albums: i32,
        /// Top-N genres by total plays (0 = not requested)
        #[arg(long, default_value_t = 0, allow_hyphen_values = true)]
        top_genres: i32,
        /// Top-N artists by total plays (0 = not requested)
        #[arg(long, default_value_t = 0, allow_hyphen_values = true)]
        top_artists: i32,
        /// Queue overview: all or id (repeatable)
        #[arg(long = "queue", value_name = "QUEUE")]
        queues: Vec<String>,
        /// Playlist overview by id (repeatable)
        #[arg(long = "playlist", value_name = "ID")]
        playlists: Vec<i64>,
        #[arg(long)]
        json: bool,
    },
    /// Add file or directory to library
    Add {
        /// File or directory path
        path: String,
        /// Recurse into subdirectories
        #[arg(long)]
        recursive: bool,
        #[arg(long)]
        json: bool,
    },
    /// Remove track from library
    Remove {
        /// Track id or path
        id: String,
        #[arg(long)]
        json: bool,
    },
    /// List all tracks in library
    List {
        /// Search query (title, artist, album, genre)
        #[arg(long, default_value = "")]
        query: String,
        #[arg(
            long,
            default_value = "",
            help = "Sort field: id, title, artist, album, year, rating, play_count, duration, date_added"
        )]
        sort: String,
        /// Sort direction: asc or desc
        #[arg(long, default_value = "")]
        order: String,
        /// Column allowlist: id,title,artist,album,genre,year,rating,duration,path
        #[arg(long, default_value = "")]
        columns: String,
        /// Filter by artist
        #[arg(long, default_value = "")]
        artist: String,
        /// Filter by album
        #[arg(long, default_value = "")]
        album: String,
        /// Filter by genre
        #[arg(long, default_value = "")]
        genre: String,
        /// Filter by user rating (0-5)
        #[arg(long, default_value_t = 0, value_parser = clap::value_parser!(i32).range(0..=5))]
        rating: i32,
        /// Limit results
        #[arg(long, default_value_t = 50)]
        limit: i32,
        /// Offset for pagination
        #[arg(long, default_value_t = 0)]
        offset: i32,
        #[arg(long)]
        json: bool,
    },
    /// List favorited (rated) tracks
    Favorites {
        /// Minimum rating (0-5)
        #[arg(
            long,
            default_value_t = 1,
            value_parser = clap::value_parser!(i32).range(0..=5)
        )]
        min_rating: i32,
        /// Limit results
        #[arg(long, default_value_t = 50)]
        limit: i32,
        #[arg(long)]
        json: bool,
    },
    /// Report or prune missing/duplicate tracks
    Maintenance {
        /// report or prune
        #[arg(default_value = "report", value_parser = ["report", "prune"])]
        action: String,
        /// Report only, no changes
        #[arg(long)]
        dry_run: bool,
        /// Library id (default: 1)
        #[arg(long, default_value_t = 1)]
        library: i64,
        #[arg(long)]
        json: bool,
    },
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn playlist_add_takes_repeatable_ids_and_paths() {
        let cli = Cli::try_parse_from([
            "kanariv",
            "playlist",
            "add",
            "3",
            "--id",
            "7",
            "--id",
            "9",
            "one.mp3",
            "two.mp3",
            "--recursive",
        ])
        .expect("parse");
        let Some(Command::Playlist {
            command:
                Some(PlaylistCommand::Add {
                    pid,
                    ids,
                    paths,
                    recursive,
                    ..
                }),
        }) = cli.command
        else {
            panic!("expected playlist add");
        };
        assert_eq!(pid, 3);
        assert_eq!(ids, ["7", "9"]);
        assert_eq!(paths, ["one.mp3", "two.mp3"]);
        assert!(recursive);
    }

    #[test]
    fn playlist_save_queue_is_optional() {
        let cli = Cli::try_parse_from(["kanariv", "playlist", "save", "mix"]).expect("parse");
        assert!(matches!(
            cli.command,
            Some(Command::Playlist {
                command: Some(PlaylistCommand::Save { queue: None, .. })
            })
        ));
    }

    #[test]
    fn playlist_export_rejects_unknown_format() {
        assert!(Cli::try_parse_from([
            "kanariv", "playlist", "export", "1", "out.txt", "--format", "txt"
        ])
        .is_err());
    }

    #[test]
    fn playlist_export_format_defaults_to_m3u() {
        let cli =
            Cli::try_parse_from(["kanariv", "playlist", "export", "1", "out.bin"]).expect("parse");
        assert!(matches!(
            cli.command,
            Some(Command::Playlist {
                command: Some(PlaylistCommand::Export { ref format, .. })
            }) if format == "m3u"
        ));
    }

    #[test]
    fn volume_accepts_negative_delta() {
        let cli = Cli::try_parse_from(["kanariv", "volume", "-10"]).expect("parse");
        assert!(matches!(
            cli.command,
            Some(Command::Volume { level: Some(ref level), .. }) if level == "-10"
        ));
    }

    #[test]
    fn seek_accepts_negative_relative_time() {
        let cli = Cli::try_parse_from(["kanariv", "seek", "-5"]).expect("parse");
        assert!(matches!(
            cli.command,
            Some(Command::Seek { ref time, .. }) if time == "-5"
        ));
    }

    #[test]
    fn library_stats_defaults_match_the_cpp_options() {
        let cli = Cli::try_parse_from(["kanariv", "library", "stats"]).expect("parse");
        assert!(matches!(
            cli.command,
            Some(Command::Library {
                command: Some(LibraryCommand::Stats {
                    top_tracks: -1,
                    top_albums: 0,
                    top_genres: 0,
                    top_artists: 0,
                    json: false,
                    ..
                })
            })
        ));
    }

    #[test]
    fn library_stats_collects_repeatable_selectors() {
        let cli = Cli::try_parse_from([
            "kanariv",
            "library",
            "stats",
            "--top-tracks",
            "5",
            "--queue",
            "all",
            "--queue",
            "3",
            "--playlist",
            "2",
        ])
        .expect("parse");
        let Some(Command::Library {
            command:
                Some(LibraryCommand::Stats {
                    top_tracks,
                    queues,
                    playlists,
                    ..
                }),
        }) = cli.command
        else {
            panic!("expected library stats");
        };
        assert_eq!(top_tracks, 5);
        assert_eq!(queues, ["all", "3"]);
        assert_eq!(playlists, [2]);
    }

    #[test]
    fn library_maintenance_action_defaults_to_report() {
        let cli = Cli::try_parse_from(["kanariv", "library", "maintenance"]).expect("parse");
        assert!(matches!(
            cli.command,
            Some(Command::Library {
                command: Some(LibraryCommand::Maintenance { ref action, library: 1, dry_run: false, json: false })
            }) if action == "report"
        ));
        assert!(Cli::try_parse_from(["kanariv", "library", "maintenance", "vacuum"]).is_err());
    }

    #[test]
    fn library_list_bounds_the_rating_filter() {
        assert!(Cli::try_parse_from(["kanariv", "library", "list", "--rating", "6"]).is_err());
        let cli = Cli::try_parse_from(["kanariv", "library", "favorites", "--min-rating", "0"])
            .expect("parse");
        assert!(matches!(
            cli.command,
            Some(Command::Library {
                command: Some(LibraryCommand::Favorites {
                    min_rating: 0,
                    limit: 50,
                    json: false
                })
            })
        ));
    }
}
