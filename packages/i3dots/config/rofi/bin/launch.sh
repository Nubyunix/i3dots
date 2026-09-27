#!/usr/bin/env bash
# launch.sh - Lanzador de aplicaciones (Type-3 Style-3)

# 1. Obtener la ruta del wallpaper estático o miniatura si es dinámico
IMAGE_PATH="$HOME/.config/i3/current_static"

# Validar que current_static exista y no sea un contenedor de video
if [[ -f "$IMAGE_PATH" ]]; then
    target=$(readlink "$IMAGE_PATH" 2>/dev/null)
    [[ "$target" =~ \.(mp4|webm|mkv|mov)$ ]] && IMAGE_PATH=""
else
    IMAGE_PATH=""
fi

# Fallback 1: color_source en state (siempre apunta a miniatura/imagen real)
if [[ -z "$IMAGE_PATH" && -f "$HOME/.config/i3dots/core/state/i3dots/wallpaper/color_source" ]]; then
    target=$(readlink "$HOME/.config/i3dots/core/state/i3dots/wallpaper/color_source" 2>/dev/null)
    if [[ ! "$target" =~ \.(mp4|webm|mkv|mov)$ ]]; then
        IMAGE_PATH="$HOME/.config/i3dots/core/state/i3dots/wallpaper/color_source"
        mkdir -p "$HOME/.config/i3"
        ln -sf "$IMAGE_PATH" "$HOME/.config/i3/current_static" 2>/dev/null
    fi
fi

# Fallback 2: Resolver desde ~/.config/i3/wall si aún no hay imagen
if [[ -z "$IMAGE_PATH" && -f "$HOME/.config/i3/wall" ]]; then
    read -r wall_path < "$HOME/.config/i3/wall"
    if [[ -f "$wall_path" ]]; then
        if [[ "$wall_path" =~ \.(mp4|webm|mkv|mov)$ ]]; then
            source "$HOME/.config/i3dots/packages/i3dots/bin/wp_shared.sh" 2>/dev/null
            if declare -F get_thumb_path &>/dev/null; then
                get_thumb_path "$wall_path" "fit"
                [[ -f "$RET_THUMB" ]] && IMAGE_PATH="$RET_THUMB"
            fi
        else
            IMAGE_PATH="$wall_path"
        fi
        if [[ -n "$IMAGE_PATH" ]]; then
            mkdir -p "$HOME/.config/i3"
            ln -sf "$IMAGE_PATH" "$HOME/.config/i3/current_static" 2>/dev/null
        fi
    fi
fi


# 2. Definir el tema
THEME="$HOME/.config/rofi/themes/style-3.rasi"

# 3. Ejecutar Rofi con la imagen de fondo dinámica en el inputbar
exec rofi -show drun -theme "$THEME" -theme-str "inputbar { background-image: url(\"$IMAGE_PATH\", width); }"
