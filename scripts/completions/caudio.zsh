# zsh completion for caudio
# Source this file in your .zshrc or copy to a zsh completion directory

_caudio() {
    local -a subcommands
    subcommands=(
        'start:Start daemon'
        'shutdown:Stop daemon'
        'play:Play current queue'
        'pause:Pause playback'
        'resume:Resume playback'
        'restart:Restart current track'
        'stop:Stop playback'
        'next:Next track'
        'prev:Prev track'
        'seek:Seek to position'
        'status:Show status'
        'volume:Get/set volume'
        'queue:Queue operations'
        'playlist:Playlist operations'
        'library:Library operations'
        'tag:Tag operations'
        'track:Track operations'
        'history:Playback history operations'
        'bookmark:Bookmark operations'
        'preview:Preview file'
        'config:Config operations'
        'device:Audio device operations'
        'info:Show current track info'
        'version:Show version and build details'
    )

    if [[ $CURRENT -eq 2 ]]; then
        _describe 'caudio subcommand' subcommands
        return
    fi

    case "$words[2]" in
        start)
            _arguments '(--foreground)-f[Run in foreground]' '--help[Print help]'
            ;;
        status)
            _arguments '--json[JSON output]' '--watch[Continuous polling]' '--follow[Alias for --watch]' '--interval=[Poll interval in ms]' '--help[Print help]'
            ;;
        volume)
            _arguments '--json[JSON output]' '--help[Print help]'
            ;;
        queue)
            local -a q_subcommands
            q_subcommands=(
                'list:List all queues'
                'tracks:List tracks in active queue'
                'switch:Switch active queue'
                'create:Create a new queue'
                'delete:Delete a queue'
                'add:Add to queue'
                'remove:Remove from queue'
                'move:Move within queue'
                'clear:Clear queue'
                'shuffle:Set shuffle'
                'repeat:Set repeat'
            )
            _describe 'queue subcommand' q_subcommands
            ;;
        playlist)
            local -a pl_subcommands
            pl_subcommands=(
                'list:List playlists'
                'tracks:Tracks in playlist'
                'create:Create an empty playlist'
                'add:Add tracks to playlist'
                'load:Load playlist into queue'
                'save:Save queue as playlist'
                'delete:Delete playlist'
                'rename:Rename playlist'
                'export:Export playlist to file'
                'import:Import playlist from file'
            )
            _describe 'playlist subcommand' pl_subcommands
            ;;
        library)
            local -a lib_subcommands
            lib_subcommands=(
                'scan:Scan library'
                'search:Search library'
                'stats:Library stats'
                'list:List all tracks in library'
                'favorites:List favorited tracks'
                'maintenance:Report or prune tracks'
                'add:Add file or directory to library'
                'remove:Remove track from library'
            )
            _describe 'library subcommand' lib_subcommands
            ;;
        tag)
            local -a tag_subcommands
            tag_subcommands=(
                'edit:Edit track tag'
                'get:Get track tags'
            )
            _describe 'tag subcommand' tag_subcommands
            ;;
        tag\ edit)
            _arguments '--json[JSON output]' '--help[Print help]'
            ;;
        tag\ get)
            _arguments '--json[JSON output]' '--out=[Output file path: :_files]' '--help[Print help]'
            ;;
        track)
            _arguments 'rate:Rate a track'
            ;;
        track\ rate)
            _arguments '--json[JSON output]' '--help[Print help]'
            ;;
        history)
            local -a h_subcommands
            h_subcommands=(
                'list:List playback history'
                'clear:Clear playback history'
            )
            _describe 'history subcommand' h_subcommands
            ;;
        history\ list)
            _arguments '--limit=[Limit entries]' '--json[JSON output]' '--help[Print help]'
            ;;
        bookmark)
            local -a bm_subcommands
            bm_subcommands=(
                'add:Add bookmark'
                'list:List bookmarks'
                'remove:Remove bookmark'
                'goto:Play and seek to bookmark'
            )
            _describe 'bookmark subcommand' bm_subcommands
            ;;
        preview)
            _arguments '--keep-playing[Do not pause a playing daemon]' '--help[Print help]'
            ;;
        config)
            local -a cfg_subcommands
            cfg_subcommands=(
                'get:Get config value'
                'set:Set config value'
                'list:List config'
                'export:Export config'
                'import:Import config'
                'reset:Reset config to defaults'
            )
            _describe 'config subcommand' cfg_subcommands
            ;;
        device)
            local -a dev_subcommands
            dev_subcommands=(
                'list:List audio output devices'
                'set:Set default audio device'
                'test:Check audio device is available'
            )
            _describe 'device subcommand' dev_subcommands
            ;;
        device\ test)
            _arguments '--id=[Device ID]' '--tone[Play an audible test tone]' '--json[JSON output]' '--help[Print help]'
            ;;
        library\ list)
            _arguments '--query=[Search query]' '--sort=[Sort field]' '--order=[Sort direction]' '--columns=[Column allowlist]' '--artist=[Filter by artist]' '--album=[Filter by album]' '--genre=[Filter by genre]' '--rating=[Filter by rating]' '--limit=[Limit results]' '--offset=[Offset for pagination]' '--json[JSON output]' '--help[Print help]'
            ;;
        library\ stats)
            _arguments '--top-tracks=[Top-N tracks]' '--top-albums=[Top-N albums]' '--top-genres=[Top-N genres]' '--top-artists=[Top-N artists]' '--queue=[Queue overview]' '--playlist=[Playlist overview]' '--json[JSON output]' '--help[Print help]'
            ;;
    esac
}

compdef _caudio caudio
