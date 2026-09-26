#!/usr/bin/env bash
# powermenu.sh - Menú de energía agnóstico y ligero para i3dots

# 1. Cargar entorno del paquete
export BASE_DIR="${BASE_DIR:-$HOME/.config/i3dots}"
export CURRENT_ENV="${CURRENT_ENV:-i3dots}"
export PACKAGE_DIR="${PACKAGE_DIR:-$BASE_DIR/packages/$CURRENT_ENV}"
[ -f "$PACKAGE_DIR/config.env" ] && source "$PACKAGE_DIR/config.env"

# 2. Configurar comandos de energía e init-system
ctl=$(command -v systemctl 2>/dev/null || command -v loginctl 2>/dev/null || command -v sudo 2>/dev/null || command -v doas 2>/dev/null || echo "")
CMD_SHUTDOWN="${POWERMENU_CMD_SHUTDOWN:-${ctl:+$ctl }poweroff}"
CMD_REBOOT="${POWERMENU_CMD_REBOOT:-${ctl:+$ctl }reboot}"
CMD_SUSPEND="${POWERMENU_CMD_SUSPEND:-${ctl:+$ctl }${ctl:+suspend}}"
[[ "$ctl" =~ (sudo|doas|^$) ]] && CMD_SUSPEND="${POWERMENU_CMD_SUSPEND:-${ctl:+$ctl }zzz}"
CMD_LOGOUT="${POWERMENU_CMD_LOGOUT:-i3-msg exit}"

# 3. Etiquetas de Rofi
L_SHUTDOWN="${POWERMENU_LABEL_SHUTDOWN:-󰐥}"
L_REBOOT="${POWERMENU_LABEL_REBOOT:-󰑓}"
L_SUSPEND="${POWERMENU_LABEL_SUSPEND:-󰖔}"
L_LOGOUT="${POWERMENU_LABEL_LOGOUT:-󰿅}"

# 4. Lanzar menú interactivo con tiempo de actividad centrado
BIN="${POWERMENU_BIN:-rofi}"
ARGS=(${POWERMENU_ARGS:--theme $HOME/.config/rofi/themes/powermenu.rasi})
UPTIME=$(uptime -p | sed -E 's/up //; s/ days?/d/g; s/ hours?/h/g; s/ minutes?/m/g; s/,//g; s/  */ /g')
THEME_STR='inputbar { children: [ "textbox-prompt-colon" ]; } textbox-prompt-colon { str: "'"$UPTIME"'"; horizontal-align: 0.5; expand: true; padding: 12px 15px; background-color: transparent; text-color: inherit; }'

CHOSEN=$(printf "%s\n" "$L_SUSPEND" "$L_LOGOUT" "$L_REBOOT" "$L_SHUTDOWN" | "$BIN" -dmenu "${ARGS[@]}" -theme-str "$THEME_STR")

case "$CHOSEN" in
    "$L_SHUTDOWN") [ -n "$CMD_SHUTDOWN" ] && eval "$CMD_SHUTDOWN" ;;
    "$L_REBOOT")   [ -n "$CMD_REBOOT" ]   && eval "$CMD_REBOOT" ;;
    "$L_SUSPEND")  [ -n "$CMD_SUSPEND" ]  && eval "$CMD_SUSPEND" ;;
    "$L_LOGOUT")   [ -n "$CMD_LOGOUT" ]   && eval "$CMD_LOGOUT" ;;
esac
