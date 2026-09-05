# Zsh quick ask: source after plugins/keymap setup in ~/.zshrc.
# Type a prompt, then Ctrl-X then a. Enter remains a shell command.
[[ -o interactive ]] || return 0

_tny_ask_widget() {
    emulate -L zsh
    # A secondary shell prompt already has parsed input outside BUFFER.
    if [[ -n $PREBUFFER ]]; then
        zle -M 'tny: cancel the unfinished shell command before asking'
        return 0
    fi
    [[ -n ${BUFFER//[[:space:]]/} ]] || return 0

    local ask_text=$BUFFER saved_cursor=$CURSOR
    local result=130
    zle -I
    {
        # No eval, shell re-parsing, history entry, argv payload, or temp file.
        # TNY_ASK_FLAGS is a Zsh array of global tny options.
        builtin printf '%s' "$ask_text" |
            command "${TNY_BIN:-tny}" "${TNY_ASK_FLAGS[@]}" ask --ephemeral --stdin
        result=$?
    } always {
        if (( result == 0 )); then
            BUFFER=''
            CURSOR=0
        else
            BUFFER=$ask_text
            CURSOR=$saved_cursor
        fi
        builtin printf '\n'
        # -I already invalidated the old editor display. Refresh at the new
        # cursor position; reset-prompt moves back to the old prompt origin
        # and overwrites answer rows when the shell prompt spans lines.
        zle -R
        if (( result != 0 )); then
            zle -M "tny: ask exited $result; prompt kept for retry (Ctrl-C to clear)"
        fi
    }
    return 0
}

zle -N tny-ask _tny_ask_widget
# Both editing styles; do not change the user's main keymap or Enter widget.
bindkey -M emacs '^Xa' tny-ask
bindkey -M viins '^Xa' tny-ask
bindkey -M vicmd '^Xa' tny-ask
