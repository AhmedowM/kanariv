# bash completion for kanariv
# Source this file in your .bashrc or copy to /etc/bash_completion.d/

_kanariv() {
    local cur prev opts
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    # Top-level subcommands
    local subcommands="start shutdown play pause resume restart stop next prev seek status volume queue playlist library tag track history bookmark preview config device info version"

    case "${prev}" in
        kanariv)
            COMPREPLY=( $(compgen -W "${subcommands}" -- ${cur}) )
            return 0
            ;;
        start)
            COMPREPLY=( $(compgen -W "--foreground --help" -- ${cur}) )
            return 0
            ;;
        status)
            COMPREPLY=( $(compgen -W "--json --watch --follow --interval --help" -- ${cur}) )
            return 0
            ;;
        volume)
            COMPREPLY=( $(compgen -W "--json --help" -- ${cur}) )
            return 0
            ;;
        queue)
            local q_subcommands="list tracks switch create delete add remove move clear shuffle repeat"
            COMPREPLY=( $(compgen -W "${q_subcommands}" -- ${cur}) )
            return 0
            ;;
        playlist)
            local pl_subcommands="list tracks create add load save delete rename export import"
            COMPREPLY=( $(compgen -W "${pl_subcommands}" -- ${cur}) )
            return 0
            ;;
        library)
            local lib_subcommands="scan search stats list favorites maintenance add remove"
            COMPREPLY=( $(compgen -W "${lib_subcommands}" -- ${cur}) )
            return 0
            ;;
        tag)
            local tag_subcommands="edit get"
            COMPREPLY=( $(compgen -W "${tag_subcommands}" -- ${cur}) )
            return 0
            ;;
        tag\ edit)
            COMPREPLY=( $(compgen -W "--json --help" -- ${cur}) )
            return 0
            ;;
        tag\ get)
            COMPREPLY=( $(compgen -W "--json --out --help" -- ${cur}) )
            return 0
            ;;
        track)
            COMPREPLY=( $(compgen -W "rate" -- ${cur}) )
            return 0
            ;;
        track\ rate)
            COMPREPLY=( $(compgen -W "--json --help" -- ${cur}) )
            return 0
            ;;
        history)
            local h_subcommands="list clear"
            COMPREPLY=( $(compgen -W "${h_subcommands}" -- ${cur}) )
            return 0
            ;;
        history\ list)
            COMPREPLY=( $(compgen -W "--limit --json --help" -- ${cur}) )
            return 0
            ;;
        bookmark)
            local bm_subcommands="add list remove goto"
            COMPREPLY=( $(compgen -W "${bm_subcommands}" -- ${cur}) )
            return 0
            ;;
        preview)
            COMPREPLY=( $(compgen -W "--keep-playing --help" -- ${cur}) )
            return 0
            ;;
        config)
            local cfg_subcommands="get set list export import reset"
            COMPREPLY=( $(compgen -W "${cfg_subcommands}" -- ${cur}) )
            return 0
            ;;
        device)
            local dev_subcommands="list set test"
            COMPREPLY=( $(compgen -W "${dev_subcommands}" -- ${cur}) )
            return 0
            ;;
        device\ test)
            COMPREPLY=( $(compgen -W "--id --tone --json --help" -- ${cur}) )
            return 0
            ;;
        library\ list)
            COMPREPLY=( $(compgen -W "--query --sort --order --columns --artist --album --genre --rating --limit --offset --json --help" -- ${cur}) )
            return 0
            ;;
        library\ stats)
            COMPREPLY=( $(compgen -W "--top-tracks --top-albums --top-genres --top-artists --queue --playlist --json --help" -- ${cur}) )
            return 0
            ;;
        *)
            COMPREPLY=()
            ;;
    esac
}

complete -F _kanariv kanariv
