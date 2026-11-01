# fish completion for caudio
# Source this file in your config.fish or copy to ~/.config/fish/completions/

complete -c caudio -f -a 'start' -d 'Start daemon'
complete -c caudio -f -a 'shutdown' -d 'Stop daemon'
complete -c caudio -f -a 'play' -d 'Play current queue'
complete -c caudio -f -a 'pause' -d 'Pause playback'
complete -c caudio -f -a 'resume' -d 'Resume playback'
complete -c caudio -f -a 'restart' -d 'Restart current track'
complete -c caudio -f -a 'stop' -d 'Stop playback'
complete -c caudio -f -a 'next' -d 'Next track'
complete -c caudio -f -a 'prev' -d 'Prev track'
complete -c caudio -f -a 'seek' -d 'Seek to position'
complete -c caudio -f -a 'status' -d 'Show status'
complete -c caudio -f -a 'volume' -d 'Get/set volume'
complete -c caudio -f -a 'queue' -d 'Queue operations'
complete -c caudio -f -a 'playlist' -d 'Playlist operations'
complete -c caudio -f -a 'library' -d 'Library operations'
complete -c caudio -f -a 'tag' -d 'Tag operations'
complete -c caudio -f -a 'track' -d 'Track operations'
complete -c caudio -f -a 'history' -d 'Playback history operations'
complete -c caudio -f -a 'bookmark' -d 'Bookmark operations'
complete -c caudio -f -a 'preview' -d 'Preview file'
complete -c caudio -f -a 'config' -d 'Config operations'
complete -c caudio -f -a 'device' -d 'Audio device operations'
complete -c caudio -f -a 'info' -d 'Show current track info'
complete -c caudio -f -a 'version' -d 'Show version and build details'

# Global options
complete -c caudio -f -l config -d 'Config file' -r
complete -c caudio -f -l log-level -d 'trace|debug|info|warn|error' -r
complete -c caudio -f -l device -d 'Audio output device' -r
complete -c caudio -f -l version -d 'Show version and exit'
complete -c caudio -f -l help -d 'Print help and exit'

# Subcommand completions
complete -c caudio -f -a 'start' -n '__fish_seen_subcommand_from caudio start shutdown play pause resume restart stop next prev seek status volume queue playlist library tag track history bookmark preview config device info version' -d 'Start daemon'
complete -c caudio -f -a 'shutdown' -n '__fish_seen_subcommand_from caudio start shutdown play pause resume restart stop next prev seek status volume queue playlist library tag track history bookmark preview config device info version' -d 'Stop daemon'

# status
complete -c caudio -f -a 'status' -n '__fish_seen_subcommand_from caudio status' -d 'Show status'
complete -c caudio -f -l json -n '__fish_seen_subcommand_from caudio status' -d 'JSON output'
complete -c caudio -f -l watch -n '__fish_seen_subcommand_from caudio status' -d 'Continuous polling'
complete -c caudio -f -l follow -n '__fish_seen_subcommand_from caudio status' -d 'Alias for --watch'
complete -c caudio -f -l interval -n '__fish_seen_subcommand_from caudio status' -d 'Poll interval in ms' -r

# library list
complete -c caudio -f -a 'library' -n '__fish_seen_subcommand_from caudio library scan search stats list favorites maintenance add remove' -d 'Library operations'
complete -c caudio -f -a 'list' -n '__fish_seen_subcommand_from caudio library list' -d 'List all tracks in library'
complete -c caudio -f -l query -n '__fish_seen_subcommand_from caudio library list' -d 'Search query' -r
complete -c caudio -f -l sort -n '__fish_seen_subcommand_from caudio library list' -d 'Sort field' -r
complete -c caudio -f -l order -n '__fish_seen_subcommand_from caudio library list' -d 'Sort direction' -r
complete -c caudio -f -l columns -n '__fish_seen_subcommand_from caudio library list' -d 'Column allowlist' -r
complete -c caudio -f -l artist -n '__fish_seen_subcommand_from caudio library list' -d 'Filter by artist' -r
complete -c caudio -f -l album -n '__fish_seen_subcommand_from caudio library list' -d 'Filter by album' -r
complete -c caudio -f -l genre -n '__fish_seen_subcommand_from caudio library list' -d 'Filter by genre' -r
complete -c caudio -f -l rating -n '__fish_seen_subcommand_from caudio library list' -d 'Filter by rating' -r
complete -c caudio -f -l limit -n '__fish_seen_subcommand_from caudio library list' -d 'Limit results' -r
complete -c caudio -f -l offset -n '__fish_seen_subcommand_from caudio library list' -d 'Offset for pagination' -r
complete -c caudio -f -l json -n '__fish_seen_subcommand_from caudio library list' -d 'JSON output'

# library stats
complete -c caudio -f -a 'stats' -n '__fish_seen_subcommand_from caudio library stats' -d 'Library stats'
complete -c caudio -f -l top-tracks -n '__fish_seen_subcommand_from caudio library stats' -d 'Top-N tracks' -r
complete -c caudio -f -l top-albums -n '__fish_seen_subcommand_from caudio library stats' -d 'Top-N albums' -r
complete -c caudio -f -l top-genres -n '__fish_seen_subcommand_from caudio library stats' -d 'Top-N genres' -r
complete -c caudio -f -l top-artists -n '__fish_seen_subcommand_from caudio library stats' -d 'Top-N artists' -r
complete -c caudio -f -l queue -n '__fish_seen_subcommand_from caudio library stats' -d 'Queue overview' -r
complete -c caudio -f -l playlist -n '__fish_seen_subcommand_from caudio library stats' -d 'Playlist overview' -r
complete -c caudio -f -l json -n '__fish_seen_subcommand_from caudio library stats' -d 'JSON output'

# tag get
complete -c caudio -f -a 'get' -n '__fish_seen_subcommand_from caudio tag get' -d 'Get track tags'
complete -c caudio -f -l json -n '__fish_seen_subcommand_from caudio tag get' -d 'JSON output'
complete -c caudio -f -l out -n '__fish_seen_subcommand_from caudio tag get' -d 'Output file path' -r

# tag edit
complete -c caudio -f -a 'edit' -n '__fish_seen_subcommand_from caudio tag edit' -d 'Edit track tag'
complete -c caudio -f -l json -n '__fish_seen_subcommand_from caudio tag edit' -d 'JSON output'

# track rate
complete -c caudio -f -a 'rate' -n '__fish_seen_subcommand_from caudio track rate' -d 'Rate a track'
complete -c caudio -f -l json -n '__fish_seen_subcommand_from caudio track rate' -d 'JSON output'

# device test
complete -c caudio -f -a 'test' -n '__fish_seen_subcommand_from caudio device test' -d 'Check audio device is available'
complete -c caudio -f -l id -n '__fish_seen_subcommand_from caudio device test' -d 'Device ID' -r
complete -c caudio -f -l tone -n '__fish_seen_subcommand_from caudio device test' -d 'Play an audible test tone'
complete -c caudio -f -l json -n '__fish_seen_subcommand_from caudio device test' -d 'JSON output'
