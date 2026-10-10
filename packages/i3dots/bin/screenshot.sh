#!/usr/bin/env bash

# Resolve pictures directory using the XDG user directory standard
pic_dir="$(xdg-user-dir PICTURES 2>/dev/null)"
if [ -z "$pic_dir" ] || [ "$pic_dir" = "$HOME" ]; then
    pic_dir="$HOME/Pictures"
fi

target_dir="$pic_dir/Screenshots"
timestamp=$(date -u +'%Y%m%d-%H%M%SZ')
tmp="/tmp/screenshot.png"

copy_to_clipboard() {
    local img="$1"
    local xic_bin="$HOME/.local/bin/xic"
    if [ -x "$xic_bin" ]; then
        "$xic_bin" "$img"
    elif command -v xclip &>/dev/null; then
        xclip -selection clipboard -t image/png "$img"
    fi
}

case "${1:-}" in
    --all)
        mkdir -p "$target_dir"
        maim --format=png "$target_dir/screenshot-${timestamp}-all.png"
        ;;
    --window)
        mkdir -p "$target_dir"
        win_id=$(xdotool getactivewindow 2>/dev/null)
        if [ -n "$win_id" ]; then
            maim --format=png --window "$win_id" "$target_dir/screenshot-${timestamp}-current.png"
        else
            maim --format=png "$target_dir/screenshot-${timestamp}-current.png"
        fi
        ;;
    --select)
        mkdir -p "$target_dir"
        maim --noopengl --format=png --select "$target_dir/screenshot-${timestamp}-selected.png"
        ;;
    --clip-all)
        if maim "$tmp"; then
            copy_to_clipboard "$tmp"
            rm -f "$tmp"
        fi
        ;;
    --clip-window)
        win_id=$(xdotool getactivewindow 2>/dev/null)
        if [ -n "$win_id" ]; then
            maim --window "$win_id" "$tmp"
        else
            maim "$tmp"
        fi
        if [ -f "$tmp" ]; then
            copy_to_clipboard "$tmp"
            rm -f "$tmp"
        fi
        ;;
    --clip-select)
        if maim --hidecursor --capturebackground --select "$tmp"; then
            copy_to_clipboard "$tmp"
            rm -f "$tmp"
        fi
        ;;
    *)
        echo "Uso: $(basename "$0") [--all | --window | --select | --clip-all | --clip-window | --clip-select]"
        exit 1
        ;;
esac
