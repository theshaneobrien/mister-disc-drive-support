#!/bin/bash
# Overlay PoC driver - exercises milestones 0/1/2 against a RUNNING GAME CORE.
#
# Run ON the MiSTer:   ssh root@<mister> 'bash -s' < overlay-poc/test.sh
# or copy anywhere on the MiSTer and run from a shell (not the OSD Scripts
# menu - that menu takes over the framebuffer we're testing).
#
# Needs the overlaypoc2+ build for the overlay_shot section (older builds
# silently ignore unknown verbs). Telemetry: /tmp/overlay_perf.log.

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

say "M1: full-color test pattern via the HPS framebuffer (replaces game for 4s)"
echo "  bar order should be: white yellow cyan green magenta red blue black"
echo "overlay_show testpat" > $CMD
sleep 4
echo "overlay_hide" > $CMD
sleep 1

say "M2a: freeze-frame, all in RAM (overlay_shot - the real pipeline path)"
echo "overlay_shot" > $CMD
sleep 4
echo "overlay_hide" > $CMD
sleep 1

say "M2b: capture -> PNG on SD -> re-display (the slow reference path)"
# wait for a NEW encode+write line, not a stale one from an earlier run,
# and require ok=1 - the save can legitimately fail
N0=$(grep -c "screenshot: encode+write" "$LOG" 2>/dev/null)
N0=${N0:-0}
rm -f "$SHOT"
echo "screenshot poc.png" > $CMD
line=""
for i in $(seq 1 150); do
    N1=$(grep -c "screenshot: encode+write" "$LOG" 2>/dev/null)
    N1=${N1:-0}
    if [ "$N1" -gt "$N0" ]; then
        line=$(grep "screenshot: encode+write" "$LOG" | tail -n 1)
        break
    fi
    sleep 0.1
done
[ -n "$line" ] || fail "screenshot never completed (no new encode+write line)"
echo "  $line"
case "$line" in
    *"ok=1"*) : ;;
    *) fail "screenshot save failed - check 'save FAILED imlib_err=' in $LOG" ;;
esac
[ -s "$SHOT" ] || fail "save reported ok but $SHOT is missing"

echo "overlay_show $SHOT" > $CMD
sleep 4
echo "overlay_hide" > $CMD
sleep 1

say "telemetry: $LOG"
tail -n 20 "$LOG"

echo
echo "PoC sequence complete. Interesting numbers:"
echo "  osd_msg render+spi        - text-over-game cost (hardware: ~1.8ms)"
echo "  screenshot read           - DDR3 frame copy (hardware: ~4ms)"
echo "  overlay_shot total        - freeze-frame on glass, RAM only"
echo "  blit(cached) vs copy(ddr) - is the fb wall the mapping or the access pattern?"
echo "  overlay_show total        - PNG-from-SD reference path"
