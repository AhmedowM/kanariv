//! Command-line surface (clap).
//!
//! Parse failures map to exit 1 (the C++ shell returns 1 from CLI11), not
//! clap's default 2. Help/version displays exit 0.

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

#[cfg(test)]
mod tests {
    use super::*;

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
}
