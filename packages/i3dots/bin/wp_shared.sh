#!/usr/bin/env bash
# packages/i3dots/bin/wp_shared.sh - Entorno y funciones compartidas para Wallpaper Helpers

# 1. Configurar Directorios Base
export BASE_DIR="${BASE_DIR:-$HOME/.config/i3dots}"
export ROOT_DIR="$BASE_DIR"
export CORE_DIR="${CORE_DIR:-$BASE_DIR/core}"
export BIN_DIR="${BIN_DIR:-$CORE_DIR/bin}"
CUR_ENV="${CURRENT_ENV:-i3dots}"
export PACKAGE_DIR="${PACKAGE_DIR:-$BASE_DIR/packages/$CUR_ENV}"
STATE_DIR_VAL="${STATE_DIR:-$BASE_DIR/core/state}"
WP_STATE_DIR="$STATE_DIR_VAL/$CUR_ENV/wallpaper"
[[ -d "$WP_STATE_DIR" ]] || mkdir -p "$WP_STATE_DIR"

# Directorio de origen de wallpapers y temas de selección
WALLPAPER_DIR="${WALLPAPER_DIR:-$HOME/wall}"
export WALL_SEL_THEME="${WALL_SEL_THEME:-$HOME/.config/rofi/themes/WallSelect.rasi}"

# Asegurar que los binarios del core y del paquete estén siempre en el PATH
export PATH="$BIN_DIR:$PACKAGE_DIR/bin:$HOME/.local/bin:$PATH"

# Migración automática de configuración heredada a state.env unificado
if [[ ! -f "$WP_STATE_DIR/state.env" ]]; then
    touch "$WP_STATE_DIR/state.env"
    for var in "show_names_mode" "card_style" "join_text" "ind_text" "ind_block" "ind_border" "ind_underline" "ind_halo" "thumbnail_mode" "thumbnail_size" "no_thumb_mode" "bg_generation" "matugen_use_thumb" "active_mode"; do
        legacy_file="$WP_STATE_DIR/$var"
        if [[ -f "$legacy_file" ]]; then
            val=""
            read -r val < "$legacy_file"
            val="${val//[[:space:]]/}"
            if [[ -n "$val" ]]; then
                echo "${var}=\"${val}\"" >> "$WP_STATE_DIR/state.env"
            fi
            rm -f "$legacy_file"
        fi
    done
    unset var legacy_file val
fi

# Cargar variables de estado unificadas
[[ -f "$WP_STATE_DIR/state.env" ]] && source "$WP_STATE_DIR/state.env"

# Helper para obtener estados guardados
get_state() {
    local key="$1"
    local default="$2"
    if [[ -n "${!key+x}" ]]; then
        echo "${!key}"
    else
        echo "$default"
    fi
}

# Helper de modo claro/oscuro: persiste y señaliza al sistema
set_theme_mode() { save_state "active_mode" "$1"; command -v gsettings &>/dev/null && gsettings set org.gnome.desktop.interface color-scheme "prefer-$1"; }

# Helper para persistir estados en un único archivo de configuración
save_state() {
    local key="$1"
    local val="$2"
    local env_file="$WP_STATE_DIR/state.env"
    
    [[ -f "$env_file" ]] || touch "$env_file"
    
    if grep -q "^${key}=" "$env_file"; then
        sed -i "s|^${key}=.*|${key}=\"${val}\"|" "$env_file"
    else
        echo "${key}=\"${val}\"" >> "$env_file"
    fi
    
    printf -v "$key" "%s" "$val"
}

# 2. Cargar/Recargar variables de configuración
load_wp_config() {
    [[ -f "$WP_STATE_DIR/state.env" ]] && source "$WP_STATE_DIR/state.env"
    THUMB_MODE=$(get_state "thumbnail_mode" "enabled")
    THUMB_SIZE=$(get_state "thumbnail_size" "")
    [[ -z "$THUMB_SIZE" ]] && THUMB_SIZE=$(get_state "thumb_size" "450")

    NO_THUMB_MODE=$(get_state "no_thumb_mode" "")
    [[ -z "$NO_THUMB_MODE" ]] && NO_THUMB_MODE=$(get_state "no_thumb" "original")

    BG_GENERATION=$(get_state "bg_generation" "true")
    MATUGEN_USE_THUMB=$(get_state "matugen_use_thumb" "true")
    
    THUMB_CROP_MODE=$(get_state "thumbnail_crop_mode" "")
    [[ -z "$THUMB_CROP_MODE" ]] && THUMB_CROP_MODE=$(get_state "thumb_crop_mode" "fit")

    MATUGEN_CLEAN_TEMP=$(get_state "matugen_clean_temp" "true")
    MATUGEN_USE_FIT=$(get_state "matugen_use_fit" "true")
    
    THUMB_DIR="$WP_STATE_DIR/thumbs/${THUMB_SIZE}_${THUMB_CROP_MODE}"
}

# Inicializar configuración
load_wp_config

# 3. Detectar Dependencias de Generación de Miniaturas
HAS_VIPS=0
HAS_MAGICK=0
HAS_FFMPEGTHUMB=0
HAS_FFMPEG=0

command -v vipsthumbnail &>/dev/null && HAS_VIPS=1
command -v magick &>/dev/null && HAS_MAGICK=1 || { command -v convert &>/dev/null && HAS_MAGICK=1; }
command -v ffmpegthumbnailer &>/dev/null && HAS_FFMPEGTHUMB=1
command -v ffmpeg &>/dev/null && HAS_FFMPEG=1

HAS_IMAGE_BACKEND=0
[[ "$HAS_VIPS" -eq 1 || "$HAS_MAGICK" -eq 1 || "$HAS_FFMPEG" -eq 1 ]] && HAS_IMAGE_BACKEND=1

# 4. Indexación por huella de contenido (Content-Addressed Thumbnails)
WP_INDEX_FILE="$WP_STATE_DIR/thumb_index"
declare -gA WP_THUMB_INDEX

load_thumb_index() {
    [[ -f "$WP_INDEX_FILE" ]] || return 0
    local cid path
    while read -r cid path; do
        [[ -n "$cid" && -n "$path" ]] && WP_THUMB_INDEX["$path"]="$cid"
    done < "$WP_INDEX_FILE"
}
load_thumb_index

get_file_id() {
    local f="$1"
    [[ -f "$f" ]] || return 1
    [[ -L "$f" ]] && f=$(readlink -f "$f")
    (head -c 65536 "$f"; tail -c 4096 "$f"; stat -c %s "$f") 2>/dev/null | sha256sum | cut -c 1-16
}

# Helper para obtener ruta física de miniatura
# Retorna en variable global RET_THUMB para evitar subshells $(...)
get_thumb_path() {
    local real_file="$1"
    [[ -L "$real_file" ]] && real_file=$(readlink -f "$real_file")
    local crop_mode="${2:-$THUMB_CROP_MODE}"
    local force_rehash="${3:-0}"
    local old_cid="${WP_THUMB_INDEX["$real_file"]}"
    local cid="$old_cid"

    if [[ -z "$cid" || "$force_rehash" -eq 1 ]] && [[ -f "$real_file" ]]; then
        local new_cid
        new_cid=$(get_file_id "$real_file")
        if [[ -n "$new_cid" ]]; then
            if [[ "$new_cid" != "$old_cid" ]]; then
                WP_THUMB_INDEX["$real_file"]="$new_cid"
                mkdir -p "$(dirname "$WP_INDEX_FILE")"
                if [[ -f "$WP_INDEX_FILE" ]]; then
                    local tmp_idx="${WP_INDEX_FILE}.tmp.$$"
                    awk -v t="$real_file" '{ idx = index($0, " "); if (idx > 0 && substr($0, idx+1) == t) next; print $0 }' "$WP_INDEX_FILE" > "$tmp_idx" 2>/dev/null && mv "$tmp_idx" "$WP_INDEX_FILE"
                fi
                printf "%s %s\n" "$new_cid" "$real_file" >> "$WP_INDEX_FILE"
            fi
            cid="$new_cid"
        fi
    fi

    if [[ -n "$cid" ]]; then
        local target_dir="$WP_STATE_DIR/thumbs/${THUMB_SIZE}_${crop_mode}"
        local thumb_file="$target_dir/${cid}.jpg"

        # Migración instantánea de miniatura heredada si el nuevo ID aún no existe físicamente
        if [[ ! -f "$thumb_file" && -d "$target_dir" ]]; then
            local legacy_name="${real_file//\//_}.jpg"
            local legacy_thumb="$target_dir/$legacy_name"
            if [[ -f "$legacy_thumb" ]]; then
                mv "$legacy_thumb" "$thumb_file" 2>/dev/null
            else
                local clean_name="${legacy_name/_noskip/}"
                clean_name=$(sed -E 's/_fps[0-9]+//' <<< "$clean_name")
                local legacy_clean="$target_dir/$clean_name"
                if [[ -f "$legacy_clean" ]]; then
                    mv "$legacy_clean" "$thumb_file" 2>/dev/null
                fi
            fi
        fi
        RET_THUMB="$thumb_file"
    else
        local safe_name="${real_file//\//_}"
        RET_THUMB="$WP_STATE_DIR/thumbs/${THUMB_SIZE}_${crop_mode}/${safe_name}.jpg"
    fi
}

# Helper universal y tolerante a fallos para generar miniatura según tipo de archivo
# Cascada de backends: libvips -> ImageMagick -> ffmpeg
generate_single_thumb() {
    local input_file="$1"
    local output_thumb="$2"
    local crop_mode="${3:-$THUMB_CROP_MODE}"

    [[ -z "$input_file" || -z "$output_thumb" ]] && return 1

    mkdir -p "$(dirname "$output_thumb")"

    if [[ "$input_file" =~ \.(mp4|webm|mkv|mov)$ ]]; then
        if [[ "$HAS_FFMPEGTHUMB" -eq 1 ]]; then
            nice -n 19 ffmpegthumbnailer -i "$input_file" -o "$output_thumb" -s "$THUMB_SIZE" &>/dev/null
            [[ -f "$output_thumb" ]] && return 0
        fi
        if [[ "$HAS_FFMPEG" -eq 1 ]]; then
            nice -n 19 ffmpeg -y -ss 00:00:01 -i "$input_file" -vframes 1 -vf "scale=${THUMB_SIZE}:-1" -q:v 2 "$output_thumb" &>/dev/null
            [[ -f "$output_thumb" ]] && return 0
        fi
    else
        # 1. libvips (máxima velocidad y eficiencia)
        if [[ "$HAS_VIPS" -eq 1 ]]; then
            local vips_args=(-s "$THUMB_SIZE")
            [[ "$crop_mode" == "crop" ]] && vips_args=(-s "${THUMB_SIZE}x${THUMB_SIZE}" -m centre)
            if nice -n 19 vipsthumbnail "${vips_args[@]}" -o "$output_thumb" "$input_file" &>/dev/null; then
                [[ -f "$output_thumb" ]] && return 0
            fi
        fi

        # 2. ImageMagick (magick / convert)
        if command -v magick &>/dev/null; then
            local geom="${THUMB_SIZE}x${THUMB_SIZE}"
            [[ "$crop_mode" == "crop" ]] && geom="${THUMB_SIZE}x${THUMB_SIZE}^ -gravity center -extent ${THUMB_SIZE}x${THUMB_SIZE}"
            if nice -n 19 magick "$input_file" -thumbnail $geom "$output_thumb" &>/dev/null; then
                [[ -f "$output_thumb" ]] && return 0
            fi
        elif command -v convert &>/dev/null; then
            local geom="${THUMB_SIZE}x${THUMB_SIZE}"
            [[ "$crop_mode" == "crop" ]] && geom="${THUMB_SIZE}x${THUMB_SIZE}^ -gravity center -extent ${THUMB_SIZE}x${THUMB_SIZE}"
            if nice -n 19 convert "$input_file" -thumbnail $geom "$output_thumb" &>/dev/null; then
                [[ -f "$output_thumb" ]] && return 0
            fi
        fi

        # 3. ffmpeg (fallback universal de imagen)
        if [[ "$HAS_FFMPEG" -eq 1 ]]; then
            local scale_arg="scale=${THUMB_SIZE}:-1"
            [[ "$crop_mode" == "crop" ]] && scale_arg="scale=${THUMB_SIZE}:${THUMB_SIZE}:force_original_aspect_ratio=increase,crop=${THUMB_SIZE}:${THUMB_SIZE}"
            if nice -n 19 ffmpeg -y -i "$input_file" -vf "$scale_arg" -q:v 2 "$output_thumb" &>/dev/null; then
                [[ -f "$output_thumb" ]] && return 0
            fi
        fi
    fi
    return 1
}

list_wallpapers() {
    if [[ "$LIVE_ONLY" -eq 1 ]]; then
        [[ -d "$WALLPAPER_DIR/live" ]] || mkdir -p "$WALLPAPER_DIR/live"
        find -L "$WALLPAPER_DIR/live" -type f \( -iname "*.gif" -o -iname "*.mp4" -o -iname "*.webm" -o -iname "*.mkv" -o -iname "*.mov" \) | sort
    else
        find -L "$WALLPAPER_DIR" -maxdepth 1 -type f \( -iname "*.jpg" -o -iname "*.jpeg" -o -iname "*.png" -o -iname "*.webp" \) | sort
    fi
}

list_all_wallpapers() {
    if [[ -d "$WALLPAPER_DIR" ]]; then
        find -L "$WALLPAPER_DIR" -maxdepth 1 -type f \( -iname "*.jpg" -o -iname "*.jpeg" -o -iname "*.png" -o -iname "*.webp" \)
    fi
    if [[ -d "$WALLPAPER_DIR/live" ]]; then
        find -L "$WALLPAPER_DIR/live" -type f \( -iname "*.gif" -o -iname "*.mp4" -o -iname "*.webm" -o -iname "*.mkv" -o -iname "*.mov" \)
    fi
}

# 5. Generador centralizado y optimizado de listas para Rofi
generate_rofi_list() {
    local line_tmpl="${1:-%f\x00icon\x1f%p}"
    local out=""
    local wallpapers_to_gen=""

    # Definir find command segun modo
    local find_cmd
    if [[ "$LIVE_ONLY" -eq 1 ]]; then
        [[ -d "$WALLPAPER_DIR/live" ]] || mkdir -p "$WALLPAPER_DIR/live"
        find_cmd=(find -L "$WALLPAPER_DIR/live" -type f \( -iname "*.gif" -o -iname "*.mp4" -o -iname "*.webm" -o -iname "*.mkv" -o -iname "*.mov" \))
    else
        find_cmd=(find -L "$WALLPAPER_DIR" -maxdepth 1 -type f \( -iname "*.jpg" -o -iname "*.jpeg" -o -iname "*.png" -o -iname "*.webp" \))
    fi

    # 1. Obtener rutas reales en bloque (un solo fork inicial)
    # 2. Bucle de procesamiento (0 forks internos usando builtins de bash)
    while IFS= read -r file; do
        [[ -z "$file" ]] && continue
        
        local thumb_to_use="$file"
        if [[ "$THUMB_MODE" == "enabled" ]]; then
            get_thumb_path "$file"
            local thumb="$RET_THUMB"
            
            # [[ -ot ]] es builtin, no hace fork
            if [[ -f "$thumb" && "$file" -ot "$thumb" ]]; then
                thumb_to_use="$thumb"
            else
                wallpapers_to_gen+="$file"$'\n'
                if [[ "$NO_THUMB_MODE" == "original" ]]; then
                    thumb_to_use="$file"
                else
                    thumb_to_use="image-x-generic"
                fi
            fi
        fi

        # Para wallpapers en live/ usar ruta relativa respecto a wall/live
        local rel_path
        if [[ "$LIVE_ONLY" -eq 1 ]]; then
            rel_path="${file#$WALLPAPER_DIR/live/}"
        else
            rel_path="${file#$WALLPAPER_DIR/}"
        fi
        
        local line="$line_tmpl"
        line="${line//%f/${file##*/}}"
        line="${line//%r/$rel_path}"
        line="${line//%p/$thumb_to_use}"
        out+="$line"$'\n'
    done < <("${find_cmd[@]}" -print0 | xargs -0 realpath | sort -u)

    # Lanzar pre-caché async de fondo (Totalmente desacoplado para no bloquear Rofi)
    if [[ "$THUMB_MODE" == "enabled" ]] && [[ "$BG_GENERATION" == "true" ]] && [[ -n "$wallpapers_to_gen" ]]; then
        wp_cache.sh --bg-gen <<< "$wallpapers_to_gen" >/dev/null 2>&1 &
        disown $! 2>/dev/null || true
    fi

    echo -ne "$out"
}


