#!/bin/bash
# Boot launcher for the MiSTer translation daemon.
#
#   /media/fat/overlay/translate_start.sh            start if ENABLED=1
#   /media/fat/overlay/translate_start.sh install    hook into boot
#   /media/fat/overlay/translate_start.sh stop       stop a running daemon
#
# 'install' appends one line to /media/fat/linux/user-startup.sh - the
# update-safe boot hook (/etc/init.d/S99User runs it; SD files survive
# Linux updates, /etc does not). Idempotent: safe to run again.

DIR=/media/fat/overlay
INI=$DIR/translate.ini
US=/media/fat/linux/user-startup.sh

case "$1" in
install)
    [ -f "$US" ] || printf '#!/bin/sh\n' > "$US"
    chmod +x "$US" 2>/dev/null
    if grep -q translate_start.sh "$US"; then
        echo "already installed in $US"
    else
        echo '[ -x /media/fat/overlay/translate_start.sh ] && /media/fat/overlay/translate_start.sh >/dev/null 2>&1 &' >> "$US"
        echo "installed into $US - daemon starts at boot when ENABLED=1 in $INI"
    fi
    exit 0
    ;;
stop)
    # a fifo write BLOCKS when nobody reads it - only talk to the pipe
    # when the daemon actually exists, and timeout in case it's wedged
    if pgrep -f translate_daemon.py >/dev/null 2>&1; then
        timeout 2 sh -c 'echo quit > /tmp/translate_cmd' 2>/dev/null
        sleep 1
        pkill -f translate_daemon.py 2>/dev/null
        echo "stopped"
    else
        echo "not running"
    fi
    exit 0
    ;;
esac

# boot path: only start when the user opted in and nothing is running
grep -q '^ENABLED=1' "$INI" 2>/dev/null || exit 0
pgrep -f translate_daemon.py >/dev/null && exit 0

exec python3 "$DIR/translate_daemon.py" --config "$INI"
