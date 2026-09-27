#!/usr/bin/env bash
# feh.sh - Motor de wallpaper estático (feh)

MPV_SOCKET="/tmp/mpv-live-wp.sock"

engine_init() {
    # Limpiar procesos de video residuales al cambiar a estático
    pkill -9 -f 'xwinwrap' &>/dev/null || true
    pkill -9 -f 'mpv.*--x11-name=mpv-wallpaper' &>/dev/null || true
    pkill -9 -f 'live_wp_daemon' &>/dev/null || true
    rm -f "$MPV_SOCKET"
}

engine_set() {
    local wp_path="$1"
    engine_init
    local wp_state_dir="${BASE_DIR:-$HOME/.config/i3dots}/core/state/${CURRENT_ENV:-i3dots}/wallpaper"
    mkdir -p "$wp_state_dir" "$HOME/.config/i3"

    # Resolver miniatura para color_source y current_static si matugen_use_thumb está activo
    local color_src="$wp_path"
    local wp_shared="${BASE_DIR:-$HOME/.config/i3dots}/packages/i3dots/bin/wp_shared.sh"
    if [[ -f "$wp_shared" ]]; then
        source "$wp_shared" 2>/dev/null
        if [[ "$MATUGEN_USE_THUMB" == "true" ]]; then
            local target_crop="${THUMB_CROP_MODE:-fit}"
            [[ "$MATUGEN_USE_FIT" == "true" ]] && target_crop="fit"
            get_thumb_path "$wp_path" "$target_crop"
            if [[ -f "$RET_THUMB" ]]; then
                color_src="$RET_THUMB"
            fi
        fi
    fi

    ln -sf "$color_src" "$wp_state_dir/color_source"
    ln -sf "$color_src" "$HOME/.config/i3/current_static"
    feh --bg-fill "$wp_path"
}
