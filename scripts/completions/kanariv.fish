# fish completion for kanariv
# Source this file in your config.fish or copy to ~/.config/fish/completions/

complete -c kanariv -f -a 'start' -d 'Start daemon'
complete -c kanariv -f -a 'shutdown' -d 'Stop daemon'
complete -c kanariv -f -a 'play' -d 'Play current queue'
complete -c kanariv -f -a 'pause' -d 'Pause playback'
complete -c kanariv -f -a 'resume' -d 'Resume playback'
complete -c kanariv -f -a 'restart' -d 'Restart current track'
complete -c kanariv -f -a 'stop' -d 'Stop playback'
complete -c kanariv -f -a 'next' -d 'Next track'
complete -c kanariv -f -a 'prev' -d 'Prev track'
complete -c kanariv -f -a 'seek' -d 'Seek to position'
complete -c kanariv -f -a 'status' -d 'Show status'
complete -c kanariv -f -a 'volume' -d 'Get/set volume'
complete -c kanariv -f -a 'queue' -d 'Queue operations'
complete -c kanariv -f -a 'playlist' -d 'Playlist operations'
complete -c kanariv -f -a 'library' -d 'Library operations'
complete -c kanariv -f -a 'tag' -d 'Tag operations'
complete -c kanariv -f -a 'track' -d 'Track operations'
complete -c kanariv -f -a 'history' -d 'Playback history operations'
complete -c kanariv -f -a 'bookmark' -d 'Bookmark operations'
complete -c kanariv -f -a 'preview' -d 'Preview file'
complete -c kanariv -f -a 'config' -d 'Config operations'
complete -c kanariv -f -a 'device' -d 'Audio device operations'
complete -c kanariv -f -a 'info' -d 'Show current track info'
complete -c kanariv -f -a 'version' -d 'Show version and build details'

# Global options
complete -c kanariv -f -l config -d 'Config file' -r
complete -c kanariv -f -l log-level -d 'trace|debug|info|warn|error' -r
complete -c kanariv -f -l device -d 'Audio output device' -r
complete -c kanariv -f -l version -d 'Show version and exit'
complete -c kanariv -f -l help -d 'Print help and exit'

# Subcommand completions
complete -c kanariv -f -a 'start' -n '__fish_seen_subcommand_from kanariv start shutdown play pause resume restart stop next prev seek status volume queue playlist library tag track history bookmark preview config device info version' -d 'Start daemon'
complete -c kanariv -f -a 'shutdown' -n '__fish_seen_subcommand_from kanariv start shutdown play pause resume restart stop next prev seek status volume queue playlist library tag track history bookmark preview config device info version' -d 'Stop daemon'

# status
complete -c kanariv -f -a 'status' -n '__fish_seen_subcommand_from kanariv status' -d 'Show status'
complete -c kanariv -f -l json -n '__fish_seen_subcommand_from kanariv status' -d 'JSON output'
complete -c kanariv -f -l watch -n '__fish_seen_subcommand_from kanariv status' -d 'Continuous polling'
complete -c kanariv -f -l follow -n '__fish_seen_subcommand_from kanariv status' -d 'Alias for --watch'
complete -c kanariv -f -l interval -n '__fish_seen_subcommand_from kanariv status' -d 'Poll interval in ms' -r

# library list
complete -c kanariv -f -a 'library' -n '__fish_seen_subcommand_from kanariv library scan search stats list favorites maintenance add remove' -d 'Library operations'
complete -c kanariv -f -a 'list' -n '__fish_seen_subcommand_from kanariv library list' -d 'List all tracks in library'
complete -c kanariv -f -l query -n '__fish_seen_subcommand_from kanariv library list' -d 'Search query' -r
complete -c kanariv -f -l sort -n '__fish_seen_subcommand_from kanariv library list' -d 'Sort field' -r
complete -c kanariv -f -l order -n '__fish_seen_subcommand_from kanariv library list' -d 'Sort direction' -r
complete -c kanariv -f -l columns -n '__fish_seen_subcommand_from kanariv library list' -d 'Column allowlist' -r
complete -c kanariv -f -l artist -n '__fish_seen_subcommand_from kanariv library list' -d 'Filter by artist' -r
complete -c kanariv -f -l album -n '__fish_seen_subcommand_from kanariv library list' -d 'Filter by album' -r
complete -c kanariv -f -l genre -n '__fish_seen_subcommand_from kanariv library list' -d 'Filter by genre' -r
complete -c kanariv -f -l rating -n '__fish_seen_subcommand_from kanariv library list' -d 'Filter by rating' -r
complete -c kanariv -f -l limit -n '__fish_seen_subcommand_from kanariv library list' -d 'Limit results' -r
complete -c kanariv -f -l offset -n '__fish_seen_subcommand_from kanariv library list' -d 'Offset for pagination' -r
complete -c kanariv -f -l json -n '__fish_seen_subcommand_from kanariv library list' -d 'JSON output'

# library stats
complete -c kanariv -f -a 'stats' -n '__fish_seen_subcommand_from kanariv library stats' -d 'Library stats'
complete -c kanariv -f -l top-tracks -n '__fish_seen_subcommand_from kanariv library stats' -d 'Top-N tracks' -r
complete -c kanariv -f -l top-albums -n '__fish_seen_subcommand_from kanariv library stats' -d 'Top-N albums' -r
complete -c kanariv -f -l top-genres -n '__fish_seen_subcommand_from kanariv library stats' -d 'Top-N genres' -r
complete -c kanariv -f -l top-artists -n '__fish_seen_subcommand_from kanariv library stats' -d 'Top-N artists' -r
complete -c kanariv -f -l queue -n '__fish_seen_subcommand_from kanariv library stats' -d 'Queue overview' -r
complete -c kanariv -f -l playlist -n '__fish_seen_subcommand_from kanariv library stats' -d 'Playlist overview' -r
complete -c kanariv -f -l json -n '__fish_seen_subcommand_from kanariv library stats' -d 'JSON output'

# tag get
complete -c kanariv -f -a 'get' -n '__fish_seen_subcommand_from kanariv tag get' -d 'Get track tags'
complete -c kanariv -f -l json -n '__fish_seen_subcommand_from kanariv tag get' -d 'JSON output'
complete -c kanariv -f -l out -n '__fish_seen_subcommand_from kanariv tag get' -d 'Output file path' -r

# tag edit
complete -c kanariv -f -a 'edit' -n '__fish_seen_subcommand_from kanariv tag edit' -d 'Edit track tag'
complete -c kanariv -f -l json -n '__fish_seen_subcommand_from kanariv tag edit' -d 'JSON output'

# track rate
complete -c kanariv -f -a 'rate' -n '__fish_seen_subcommand_from kanariv track rate' -d 'Rate a track'
complete -c kanariv -f -l json -n '__fish_seen_subcommand_from kanariv track rate' -d 'JSON output'

# device test
complete -c kanariv -f -a 'test' -n '__fish_seen_subcommand_from kanariv device test' -d 'Check audio device is available'
complete -c kanariv -f -l id -n '__fish_seen_subcommand_from kanariv device test' -d 'Device ID' -r
complete -c kanariv -f -l tone -n '__fish_seen_subcommand_from kanariv device test' -d 'Play an audible test tone'
complete -c kanariv -f -l json -n '__fish_seen_subcommand_from kanariv device test' -d 'JSON output'
