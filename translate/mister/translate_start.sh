#!/bin/bash
# Boot launcher for the MiSTer translation daemon.
#
#   /media/fat/translate/translate_start.sh            start if ENABLED=1
#   /media/fat/translate/translate_start.sh install    hook into boot
#   /media/fat/translate/translate_start.sh stop       stop a running daemon
#
# 'install' appends one line to /media/fat/linux/user-startup.sh - the
# update-safe boot hook (/etc/init.d/S99User runs it; SD files survive
# Linux updates, /etc does not). Idempotent: safe to run again.
#
# Busybox-safe: the MiSTer rootfs has no pgrep/pkill (and maybe no
# timeout), so process checks scan /proc directly.

DIR=/media/fat/translate
INI=$DIR/translate.ini
US=/media/fat/linux/user-startup.sh
FIFO=/tmp/translate_cmd

daemon_pids() {
    for d in /proc/[0-9]*; do
        grep -qs "translate_daemon\.py" "$d/cmdline" 2>/dev/null && echo "${d##*/}"
    done
}

fifo_send() {
    # a fifo write BLOCKS when nobody reads it - callers must check
    # daemon_pids first; timeout (when present) guards a wedged daemon
    if command -v timeout >/dev/null 2>&1; then
        timeout 2 sh -c "echo '$1' > $FIFO" 2>/dev/null
    else
        echo "$1" > "$FIFO" 2>/dev/null
    fi
}

case "$1" in
install)
    [ -f "$US" ] || printf '#!/bin/sh\n' > "$US"
    chmod +x "$US" 2>/dev/null
    if grep -q translate_start.sh "$US"; then
        echo "already installed in $US"
    else
        echo '[ -x /media/fat/translate/translate_start.sh ] && /media/fat/translate/translate_start.sh >/dev/null 2>&1 &' >> "$US"
        echo "installed into $US - daemon starts at boot when ENABLED=1 in $INI"
    fi
    exit 0
    ;;
stop)
    if [ -n "$(daemon_pids)" ]; then
        fifo_send quit
        sleep 1
        PIDS=$(daemon_pids)
        [ -n "$PIDS" ] && kill $PIDS 2>/dev/null
        echo "stopped"
    else
        echo "not running"
    fi
    exit 0
    ;;
esac

# boot path: only start when the user opted in and nothing is running
grep -q '^ENABLED=1' "$INI" 2>/dev/null || exit 0
[ -n "$(daemon_pids)" ] && exit 0

exec python3 "$DIR/translate_daemon.py" --config "$INI"
