#!/bin/sh
# usb_nosleep.sh - stop linux autosuspending the usb cd drive
#
# an idle usb optical drive gets autosuspended by the kernel; the next
# read has to resume it, and a slow/failed resume shows up as
# "reset high-speed USB device" in dmesg and a multi-minute stall in
# the game. this pins the drive's usb device (and its parent hub port)
# to always-on.
#
# run on the mister:   sh usb_nosleep.sh
# make it stick:       add that line to /media/fat/linux/user-startup.sh
#
# reports what it found either way, so it doubles as the diagnostic.

found=0
for d in /sys/bus/usb/devices/*; do
	[ -f "$d/idVendor" ] || continue

	# does this usb device own an sr* block device?
	if ! ls "$d"/host*/target*/*/block/sr* >/dev/null 2>&1; then
		continue
	fi

	found=1
	vid=$(cat "$d/idVendor" 2>/dev/null)
	pid=$(cat "$d/idProduct" 2>/dev/null)
	echo "cd drive at $(basename "$d")  ($vid:$pid)"

	if [ -f "$d/power/control" ]; then
		echo "  power/control was: $(cat "$d/power/control")"
		echo on > "$d/power/control" 2>/dev/null \
			&& echo "  power/control now: $(cat "$d/power/control")" \
			|| echo "  could not write power/control"
	fi

	if [ -f "$d/power/autosuspend_delay_ms" ]; then
		echo "  autosuspend_delay_ms: $(cat "$d/power/autosuspend_delay_ms")"
	fi

	# the hub port upstream can suspend independently
	parent=$(dirname "$d")
	if [ -f "$parent/power/control" ]; then
		echo "  parent $(basename "$parent")/power/control was: $(cat "$parent/power/control")"
		echo on > "$parent/power/control" 2>/dev/null
	fi
done

[ "$found" = 1 ] || echo "no usb cd drive found under /sys/bus/usb/devices"
