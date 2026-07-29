#!/bin/bash
# SetTranslateHotkey - set the translation hotkey from the MiSTer Scripts menu.
#
# Run it from the OSD Scripts menu. While a script runs, Main releases its
# exclusive input grab, so this can listen to the controller/keyboard
# directly: press the button (or hold one and press a second for a combo)
# and it writes /media/fat/translate/hotkey.cfg for you.
# Takes effect the next time a core is loaded.
#
# The translate daemon is paused while this listens - otherwise pressing
# the CURRENT hotkey during setup fires a real translation of the script
# terminal (seen on hardware as ztranslate HTTP 500s).

CFG=/media/fat/translate/hotkey.cfg
FIFO=/tmp/translate_cmd
mkdir -p /media/fat/translate

# busybox-safe process check: the MiSTer rootfs has no pgrep
daemon_running() {
    for d in /proc/[0-9]*; do
        grep -qs "translate_daemon\.py" "$d/cmdline" 2>/dev/null && return 0
    done
    return 1
}

fifo_send() {
    if command -v timeout >/dev/null 2>&1; then
        timeout 2 sh -c "echo '$1' > $FIFO" 2>/dev/null
    else
        echo "$1" > "$FIFO" 2>/dev/null
    fi
}

PAUSED=
if daemon_running; then
    fifo_send "pause 120" && PAUSED=1
fi
trap '[ -n "$PAUSED" ] && fifo_send resume' EXIT

echo "=== Translation hotkey setup ==="
if [ -f "$CFG" ]; then
    echo "Current setting: $(head -1 "$CFG")"
fi
[ -n "$PAUSED" ] && echo "(translate daemon paused while you choose)"
echo ""
echo "Press the button you want as your translate hotkey."
echo "For a COMBO: hold the first button, then press the second."
echo "Keyboard keys work too. Gyro/sticks are ignored."
echo ""
echo "Waiting 20 seconds... (press nothing to keep the current setting)"
echo ""

python3 - "$CFG" <<'PYEOF'
import glob, os, select, struct, sys, time

CFG = sys.argv[1]
EV_KEY = 0x01
FMT = "llHHi"  # 32-bit armhf input_event: timeval(2*long) + type + code + value
SZ = struct.calcsize(FMT)

NAMES = {
    304: "BTN_A/SOUTH", 305: "BTN_B/EAST", 307: "BTN_X/NORTH", 308: "BTN_Y/WEST",
    310: "BTN_L", 311: "BTN_R", 312: "BTN_L2", 313: "BTN_R2",
    314: "BTN_SELECT", 315: "BTN_START", 316: "BTN_MODE/HOME",
    317: "BTN_L3", 318: "BTN_R3",
    59: "F1", 60: "F2", 61: "F3", 62: "F4", 63: "F5", 64: "F6",
    65: "F7", 66: "F8", 67: "F9", 68: "F10", 87: "F11", 88: "F12",
    70: "SCROLLLOCK", 119: "PAUSE",
}
def name(c):
    return "%s (%d)" % (NAMES.get(c, "code"), c) if c in NAMES else "code %d" % c

fds = []
for path in sorted(glob.glob("/dev/input/event*")):
    try:
        fds.append(os.open(path, os.O_RDONLY | os.O_NONBLOCK))
    except OSError:
        pass
if not fds:
    print("No input devices found.")
    sys.exit(1)

def drain():
    for fd in fds:
        try:
            while os.read(fd, SZ * 64):
                pass
        except (BlockingIOError, OSError):
            pass

def presses(timeout):
    """Yield codes for every EV_KEY press until timeout."""
    end = time.monotonic() + timeout
    while True:
        left = end - time.monotonic()
        if left <= 0:
            return
        r, _, _ = select.select(fds, [], [], min(left, 0.25))
        for fd in r:
            try:
                buf = os.read(fd, SZ * 64)
            except (BlockingIOError, OSError):
                continue
            for i in range(0, len(buf) - SZ + 1, SZ):
                _, _, etype, code, value = struct.unpack(FMT, buf[i:i + SZ])
                if etype == EV_KEY and value == 1 and code:
                    yield code

# settle: flush the button press that launched this script
time.sleep(1.0)
drain()

first = None
for code in presses(20.0):
    first = code
    break

if first is None:
    print("Nothing pressed - keeping the current setting.")
    sys.exit(0)

print("Got: %s" % name(first), flush=True)
print("...press a second button within 1.5s to make it a combo", flush=True)

second = None
for code in presses(1.5):
    if code != first:
        second = code
        break

if second is not None:
    line = "%d+%d" % (first, second)
    human = "%s + %s" % (name(first), name(second))
else:
    line = "%d" % first
    human = name(first)

with open(CFG, "w") as f:
    f.write(line + "\n")

print("")
print("Saved: %s" % human)
print("-> %s = %s" % (CFG, line))
print("")
print("Takes effect the next time you load a core.")
PYEOF
