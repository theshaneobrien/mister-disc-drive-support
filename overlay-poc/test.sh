#!/bin/bash
# Overlay PoC driver - exercises milestones 0/1/2 against a RUNNING GAME CORE.
#
# Run ON the MiSTer:   ssh root@<mister> 'bash -s' < overlay-poc/test.sh
# or copy to /media/fat/Scripts and run from a shell (not the OSD Scripts
# menu - that menu takes over the framebuffer we're testing).
#
# Prereqs: a game core running (NOT the menu core), overlay-poc build of
# /media/fat/MiSTer. Telemetry lands in /tmp/overlay_perf.log.

CMD=/dev/MiSTer_cmd
LOG=/tmp/overlay_perf.log
SHOT=/media/fat/screenshots/poc.png

say() { echo; echo "== $*"; }
fail() { echo "FAIL: $*"; exit 1; }

[ -p "$CMD" ] || fail "no $CMD - is MiSTer running?"

say "M0: text over the live game (OSD info window)"
echo "osd_msg Hello from the translation PoC" > $CMD
sleep 3
echo 'osd_msg -f 1 -t 5000 Translated:\n\nHERO: I found the\nancient sword!' > $CMD
sleep 5
# position experiment: same units as Info()'s default 20,10 - probe where
# a subtitle bar would sit on this core/video mode
echo "osd_msg -x 4 -y 200 -t 3000 subtitle position probe (-y 200)" > $CMD
sleep 4

say "M1: full-color test pattern via the HPS framebuffer (replaces game for 4s)"
echo "  bar order should be: white yellow cyan green magenta red blue black"
echo "  (if red/blue are swapped, the ARGB/RxB assumption is wrong - note it!)"
echo "overlay_show testpat" > $CMD
sleep 4
echo "overlay_hide" > $CMD
sleep 1

say "M2: capture -> re-display round trip"
rm -f "$SHOT"
echo "screenshot poc.png" > $CMD

# the encode+write perf line is the reliable "file is complete" signal
for i in $(seq 1 100); do
    grep -q "encode+write.*poc.png" "$LOG" 2>/dev/null && break
    sleep 0.1
done
[ -s "$SHOT" ] || fail "screenshot never appeared at $SHOT"

echo "overlay_show $SHOT" > $CMD
sleep 4
echo "overlay_hide" > $CMD
sleep 1

say "telemetry: $LOG"
tail -n 20 "$LOG"

echo
echo "PoC sequence complete. Interesting numbers:"
echo "  osd_msg render+spi   - text-over-game cost (should be ~1-2ms)"
echo "  screenshot read      - DDR3 frame copy (NEON; ~3-5ms per Screenshot_MiSTer)"
echo "  screenshot total     - request->PNG-on-SD (includes vsync wait + PNG encode)"
echo "  overlay_show total   - draw + scaler switch + vsync (the 'on glass' bound)"
