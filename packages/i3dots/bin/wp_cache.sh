#!/usr/bin/env bash
# packages/i3dots/bin/wp_cache.sh - Helper de administración de caché de wallpapers (Backend)

# 1. Parseo de argumentos
CACHE_NOW=0
CLEAN_CACHE=0
CLEAN_ARG=""
BG_GEN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -CN|--cache-now) CACHE_NOW=1; shift ;;
        -CC|--clean-cache)
            CLEAN_CACHE=1
            CLEAN_ARG="$2"
            shift; [[ $# -gt 0 ]] && shift
            ;;
        --bg-gen) BG_GEN=1; shift ;;
        *) shift ;;
    esac
done

# 2. Cargar entorno y lógica compartida
BASE_DIR="${BASE_DIR:-$HOME/.config/i3dots}"
source "$BASE_DIR/packages/i3dots/bin/wp_shared.sh"

# 2.5 Lock de seguridad para evitar concurrencia
LOCK_FILE="/dev/shm/wp_cache_${UID}.lock"
exec 9>"$LOCK_FILE"
if ! flock -n 9; then
    # Si ya hay una instancia corriendo, salir en silencio
    exit 0
fi

# Nota: generate_single_thumb es provisto por wp_shared.sh con soporte para libvips, ImageMagick y ffmpeg.

## 3. Modo: Pre-caché en background (--bg-gen)
if [[ "$BG_GEN" -eq 1 ]]; then
    if [[ ! -t 0 ]]; then
        wallpapers_found=$(cat)
    else
        wallpapers_found=$(list_wallpapers)
    fi
    [[ -z "$wallpapers_found" ]] && exit 0
    
    [[ -d "$THUMB_DIR" ]] || mkdir -p "$THUMB_DIR"
    
    # Bucle secuencial de baja prioridad para wallpapers faltantes
    while IFS= read -r file; do
        [[ -z "$file" ]] && continue
        if [[ -L "$file" ]]; then
            real_file=$(readlink -f "$file")
        else
            real_file="$file"
        fi
        get_thumb_path "$real_file"
        thumb="$RET_THUMB"
        if [[ ! -f "$thumb" || "$real_file" -nt "$thumb" ]]; then
            if [[ "$real_file" -nt "$thumb" ]]; then
                get_thumb_path "$real_file" "$THUMB_CROP_MODE" 1
                thumb="$RET_THUMB"
            fi
            generate_single_thumb "$real_file" "$thumb"
        fi
    done <<< "$wallpapers_found"
    exit 0
fi

# 4. Modo: Cachear Ahora (--cache-now)
if [[ "$CACHE_NOW" -eq 1 ]]; then
    wallpapers_found=$(list_all_wallpapers | sort -u)
    [[ -z "$wallpapers_found" ]] && { echo "No se encontraron wallpapers." >&2; exit 0; }
    
    [[ -d "$THUMB_DIR" ]] || mkdir -p "$THUMB_DIR"
    
    # Filtrar imágenes pendientes
    mapfile -t files <<< "$wallpapers_found"
    declare -a pending=()
    for file in "${files[@]}"; do
        [[ -z "$file" ]] && continue
        if [[ -L "$file" ]]; then
            real_file=$(readlink -f "$file")
        else
            real_file="$file"
        fi
        get_thumb_path "$real_file"
        thumb="$RET_THUMB"
        if [[ ! -f "$thumb" || "$real_file" -nt "$thumb" ]]; then
            pending+=("$real_file")
        fi
    done
    
    total="${#pending[@]}"
    if [[ "$total" -eq 0 ]]; then
        echo "Caché al día. No hay miniaturas pendientes."
        exit 0
    fi
    
    echo "Generando caché de miniaturas (Calidad: ${THUMB_SIZE}px) para $total wallpapers..."
    count=0
    for file in "${pending[@]}"; do
        count=$((count+1))
        echo -e "\e[1A\e[K[$count/$total] Procesando: ${file##*/}"
        get_thumb_path "$file" "$THUMB_CROP_MODE" 1
        thumb="$RET_THUMB"
        generate_single_thumb "$file" "$thumb"
    done
    
    echo "Caché de miniaturas completado."
    exit 0
fi


# 5. Modo: Limpiar Caché (--clean-cache)
if [[ "$CLEAN_CACHE" -eq 1 ]]; then
    root_thumbs="$WP_STATE_DIR/thumbs"
    [[ ! -d "$root_thumbs" ]] && { echo "Caché vacía. Nada que limpiar." >&2; exit 0; }
    
    case "$CLEAN_ARG" in
        orphans)
            echo "Buscando miniaturas huérfanas en todas las calidades..."
            wallpapers_found=$(list_all_wallpapers | sort -u)
            if [[ -z "$wallpapers_found" ]]; then
                echo "Advertencia: No se detectaron wallpapers activos. Abortando limpieza para evitar pérdida de datos." >&2
                exit 1
            fi
            
            declare -A active_thumbs
            declare -a new_index_lines=()
            
            while IFS= read -r file; do
                [[ -z "$file" || ! -f "$file" ]] && continue
                if [[ -L "$file" ]]; then
                    real_file=$(readlink -f "$file")
                else
                    real_file="$file"
                fi
                
                # Resuelve ruta, ejecuta migración heredada y asegura CID
                get_thumb_path "$real_file"
                cid="${WP_THUMB_INDEX["$real_file"]}"
                if [[ -n "$cid" ]]; then
                    new_index_lines+=("$cid $real_file")
                    active_thumbs["${cid}.jpg"]=1
                fi
            done <<< "$wallpapers_found"
            
            # Reescribir thumb_index limpio sin entradas obsoletas
            printf "%s\n" "${new_index_lines[@]}" > "$WP_INDEX_FILE"
            
            deleted_count=0
            while IFS= read -r -d '' thumb_file; do
                [[ -z "$thumb_file" ]] && continue
                t_name="${thumb_file##*/}"
                if [[ -z "${active_thumbs["$t_name"]}" ]]; then
                    rm -f "$thumb_file"
                    deleted_count=$((deleted_count+1))
                fi
            done < <(find "$root_thumbs" -type f -name "*.jpg" -print0 2>/dev/null)
            
            echo "Limpieza completada. Borradas $deleted_count miniaturas huérfanas."
            ;;
        300|450|600|[0-9]*)
            found=0
            for d in "$root_thumbs"/${CLEAN_ARG} "$root_thumbs"/${CLEAN_ARG}_*; do
                if [[ -d "$d" ]]; then
                    rm -rf "$d"
                    found=1
                fi
            done
            if [[ "$found" -eq 1 ]]; then
                echo "Caché de calidad $CLEAN_ARG px eliminada."
            else
                echo "No existe caché para la calidad $CLEAN_ARG px."
            fi
            ;;
        keep-active)
            echo "Eliminando todas las calidades excepto la activa (${THUMB_SIZE}px)..."
            while IFS= read -r -d '' dir; do
                [[ -z "$dir" ]] && continue
                dir_name="${dir##*/}"
                if [[ "$dir_name" != "${THUMB_SIZE}" && "$dir_name" != "${THUMB_SIZE}_"* ]]; then
                    rm -rf "$dir"
                    echo "Eliminada calidad residual: $dir_name"
                fi
            done < <(find "$root_thumbs" -mindepth 1 -maxdepth 1 -type d -print0 2>/dev/null)
            ;;
        full)
            echo "Vaciando toda la caché de miniaturas..."
            rm -rf "$root_thumbs"
            rm -f "$WP_INDEX_FILE"
            echo "Caché completa eliminada."
            ;;
        *)
            echo "Opción de limpieza no válida. Opciones: orphans, Baja (300), Media (450), Alta (600), entero, keep-active, full" >&2
            exit 1
            ;;
    esac
    exit 0
fi

echo "Error: wp_cache.sh requiere --cache-now, --clean-cache o --bg-gen" >&2
exit 1
