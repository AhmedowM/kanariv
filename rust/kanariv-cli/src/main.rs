//! Entry point: parse arguments, resolve config, dispatch, render.
//!
//! The render pipeline mirrors `detail::render` in `cli/src/shell/shell.cpp`:
//! error, failure line, quiet success, JSON payload, human line, bare payload.

mod app;
mod cli;
mod client;
mod config;
mod daemon;
mod error;
mod hash;
mod ipc;
mod outcome;
mod output;
mod parse;
mod version;

use std::io::IsTerminal;
use std::process::ExitCode;

use clap::error::ErrorKind;
use clap::{CommandFactory, Parser};

use crate::app::App;
use crate::cli::{Cli, Command, QueueCommand, StatusArgs};
use crate::error::{Error, Result};
use crate::outcome::Outcome;
use crate::parse::VolumeArg;

fn main() -> ExitCode {
    ExitCode::from(u8::try_from(run()).unwrap_or(1))
}

fn run() -> i32 {
    let cli = match Cli::try_parse() {
        Ok(cli) => cli,
        Err(err) => {
            let success = matches!(
                err.kind(),
                ErrorKind::DisplayHelp
                    | ErrorKind::DisplayVersion
                    | ErrorKind::DisplayHelpOnMissingArgumentOrSubcommand
            );
            let _ = err.print();
            return i32::from(!success);
        }
    };

    let Cli {
        config,
        log_level,
        device,
        version: show_version,
        command,
    } = cli;

    if show_version {
        println!("{}", version::FULL);
        return 0;
    }

    let Some(command) = command else {
        return print_root_help();
    };

    // `version` runs before config loading so it works from any directory.
    let command = match command {
        Command::Version { json } => return version::render(json),
        other => other,
    };

    if let Some(level) = log_level.as_deref() {
        if !config::is_valid_log_level(level) {
            eprintln!("--log-level: invalid '{level}' (expected trace|debug|info|warn|error)");
            return 1;
        }
    }

    let cfg = match config::resolve(config.as_deref(), log_level.as_deref(), device.as_deref()) {
        Ok(cfg) => cfg,
        Err(err) => return render_error(&err),
    };

    let app = App::new(cfg);
    match command {
        Command::Version { .. } => 0,
        Command::Start { foreground } => render(daemon::start(app.config(), foreground), false),
        Command::Shutdown => render_outcome(&daemon::shutdown(app.config()), false),
        Command::Play { json } => render(app.play(), json),
        Command::Pause { json } => render(app.pause(), json),
        Command::Resume { json } => render(app.resume(), json),
        Command::Restart { json } => render(app.restart(), json),
        Command::Stop { json } => render(app.stop(), json),
        Command::Next { json } => render(app.next(), json),
        Command::Prev { json } => render(app.prev(), json),
        Command::Seek { time, json } => match parse::parse_seek(&time) {
            Ok((target, relative)) => render(app.seek(target, relative), json),
            Err(message) => {
                eprintln!(
                    "seek: '{time}': {message} (expected mm:ss, hh:mm:ss, seconds, or +N/-N)"
                );
                1
            }
        },
        Command::Status(args) => status(&app, &args),
        Command::Volume { level, json } => {
            match parse::parse_volume(level.as_deref().unwrap_or("")) {
                Ok(VolumeArg::Show) => render(app.set_volume(None, None, None), json),
                Ok(VolumeArg::Set {
                    level,
                    mute,
                    delta_pct,
                }) => render(app.set_volume(level, mute, delta_pct), json),
                Err(message) => {
                    eprintln!(
                        "volume: '{}': {message} (expected 0-100, +N/-N, mute, or unmute)",
                        level.as_deref().unwrap_or("")
                    );
                    1
                }
            }
        }
        Command::Queue { command } => match command {
            Some(command) => dispatch_queue(&app, command),
            None => print_queue_help(),
        },
        Command::Info { json } => render(app.info(), json),
    }
}

fn status(app: &App, args: &StatusArgs) -> i32 {
    if args.watch || args.follow {
        if args.interval <= 0 {
            eprintln!(
                "status: --interval must be positive (got {})",
                args.interval
            );
            return 1;
        }
        // The watch loop belongs to a status-stream API that this frontend
        // phase does not have yet; fail loudly instead of polling silently.
        eprintln!("status: --watch is not supported by the Rust CLI");
        return 1;
    }
    render(app.status(), args.json)
}

fn dispatch_queue(app: &App, command: QueueCommand) -> i32 {
    match command {
        QueueCommand::List { json } | QueueCommand::Queues { json } => {
            render(app.queue_list(), json)
        }
        QueueCommand::Tracks { order, json } => match app.queue_tracks(&order) {
            Err(err) => render_error(&err),
            Ok((payload, highlight)) => {
                if json {
                    output::print_json(&payload);
                } else {
                    output::print_payload(&payload, highlight, use_color());
                }
                0
            }
        },
        QueueCommand::Switch { qid, json } => render(app.queue_switch(qid), json),
        QueueCommand::Create { name, json } => render(app.queue_create(&name), json),
        QueueCommand::Delete { qid, json } => render(app.queue_delete(qid), json),
        QueueCommand::Clear { json } => render(app.queue_clear(), json),
        QueueCommand::Shuffle { mode, json } => render(app.queue_shuffle(mode.as_deref()), json),
        QueueCommand::Repeat { mode, json } => render(app.queue_repeat(mode.as_deref()), json),
    }
}

fn render(result: Result<Outcome>, as_json: bool) -> i32 {
    match result {
        Ok(outcome) => render_outcome(&outcome, as_json),
        Err(err) => render_error(&err),
    }
}

fn render_outcome(outcome: &Outcome, as_json: bool) -> i32 {
    if outcome.failed {
        if let Some(line) = &outcome.line {
            eprintln!("{line}");
        }
        return 1;
    }
    if outcome.quiet && !as_json {
        return 0;
    }
    if as_json {
        if let Some(payload) = &outcome.payload {
            output::print_json(payload);
            return 0;
        }
    }
    if let Some(line) = &outcome.line {
        if outcome.to_stderr {
            eprintln!("{line}");
        } else {
            println!("{line}");
        }
        return 0;
    }
    if let Some(payload) = &outcome.payload {
        output::print_payload(payload, 0, use_color());
    }
    0
}

fn render_error(err: &Error) -> i32 {
    eprintln!("{}", err.render());
    err.exit_code()
}

fn use_color() -> bool {
    if std::env::var_os("NO_COLOR").is_some() {
        return false;
    }
    std::io::stdout().is_terminal()
}

fn print_root_help() -> i32 {
    let mut command = Cli::command();
    let _ = command.print_help();
    println!();
    0
}

fn print_queue_help() -> i32 {
    let mut command = Cli::command();
    if let Some(queue) = command.find_subcommand_mut("queue") {
        let _ = queue.print_help();
        println!();
    }
    0
}
