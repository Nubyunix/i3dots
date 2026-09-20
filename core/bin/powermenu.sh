#!/usr/bin/env bash
# powermenu.sh - Módulo core "inteligentemente tonto"
# Gestiona las opciones de apagado/reinicio de forma agnóstica.

# 1. Configuración del paquete (con fallbacks autónomos)
BIN="${POWERMENU_BIN:-rofi}"
ARGS=(${POWERMENU_ARGS:--theme $HOME/.config/rofi/themes/powermenu.rasi})

# Etiquetas (Labels)
L_SHUTDOWN="${POWERMENU_LABEL_SHUTDOWN:-󰐥}"
L_REBOOT="${POWERMENU_LABEL_REBOOT:-󰑓}"
L_SUSPEND="${POWERMENU_LABEL_SUSPEND:-󰖔}"
L_LOGOUT="${POWERMENU_LABEL_LOGOUT:-󰿅}"

CMD_SHUTDOWN="${POWERMENU_CMD_SHUTDOWN:-systemctl poweroff}"
CMD_REBOOT="${POWERMENU_CMD_REBOOT:-systemctl reboot}"
CMD_SUSPEND="${POWERMENU_CMD_SUSPEND:-systemctl suspend}"
CMD_LOGOUT="${POWERMENU_CMD_LOGOUT:-i3-msg exit}"

# 2. Control de Flujo Rofi
if [[ -z "$ROFI_LIST_MODE" && $# -eq 0 ]]; then
    # Fase 1: Lanzar Rofi (reemplaza proceso actual)
    export ROFI_LIST_MODE=1
    UPTIME=$(uptime -p | sed -E 's/up //; s/ days?/d/g; s/ hours?/h/g; s/ minutes?/m/g; s/,//g; s/  */ /g')
    exec "$BIN" -show " " -modi " :$0" "${ARGS[@]}" -theme-str 'inputbar { children: [ "textbox-prompt-colon" ]; } textbox-prompt-colon { str: "'"$UPTIME"'"; horizontal-align: 0.5; expand: true; padding: 12px 15px; background-color: transparent; text-color: inherit; }' -p ""

elif [[ "$ROFI_LIST_MODE" -eq 1 && $# -eq 0 ]]; then
    # Fase 2: Rofi solicita lista (stdout)
    UPTIME=$(uptime -p | sed -E 's/up //; s/ days?/d/g; s/ hours?/h/g; s/ minutes?/m/g; s/,//g; s/  */ /g')
    echo -e "$L_SUSPEND\n$L_LOGOUT\n$L_REBOOT\n$L_SHUTDOWN"
    exit 0
else
    # Fase 3: Rofi devuelve selección ($1)
    CHOSEN="$1"
    [[ -z "$CHOSEN" ]] && exit 1

    case "$CHOSEN" in
        "$L_SHUTDOWN") [ -n "$CMD_SHUTDOWN" ] && eval "$CMD_SHUTDOWN" ;;
        "$L_REBOOT")   [ -n "$CMD_REBOOT" ]   && eval "$CMD_REBOOT" ;;
        "$L_SUSPEND")  [ -n "$CMD_SUSPEND" ]  && eval "$CMD_SUSPEND" ;;
        "$L_LOGOUT")   [ -n "$CMD_LOGOUT" ]   && eval "$CMD_LOGOUT" ;;
    esac
    exit 0
fi
