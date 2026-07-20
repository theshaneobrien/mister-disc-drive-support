#!/bin/sh
# usb_nosleep.sh - stop linux autosuspending the usb cd drive
#
# an idle usb optical drive gets autosuspended by the kernel; the next
# read has to resume it, and a slow/failed resume shows up as
# "reset high-speed USB device" in dmesg and a long stall in the game.
# this pins the drive's usb device (and its parent hub) to always-on.
#
# run on the mister:   sh usb_nosleep.sh
# make it stick:       add that line to /media/fat/linux/user-startup.sh
#
# it prints the BEFORE values, so it doubles as the diagnostic: if
# power/control reads "auto", autosuspend was live.

found=0

# walk BACKWARDS from each sr* block device to its usb parent. going
# forwards from /sys/bus/usb/devices is fragile - the scsi host sits
# under the usb INTERFACE dir (1-1.7:1.0/host0/...), not under the
# device dir, which is what broke the first version of this script.
for sr in /sys/block/sr*; do
	[ -e "$sr" ] || continue
	name=$(basename "$sr")

	dev=$(readlink -f "$sr/device" 2>/dev/null)
	[ -n "$dev" ] || continue

	# climb until we hit the directory holding idVendor: that is the
	# usb device node for this drive
	d="$dev"
	while [ -n "$d" ] && [ "$d" != "/" ]; do
		[ -f "$d/idVendor" ] && break
		d=$(dirname "$d")
	done

	if [ -z "$d" ] || [ ! -f "$d/idVendor" ]; then
		echo "$name: could not find its usb parent (not a usb drive?)"
		continue
	fi

	found=1
	model=$(cat "$sr/device/model" 2>/dev/null)
	echo "$name -> usb $(basename "$d")  $(cat "$d/idVendor"):$(cat "$d/idProduct")  $model"

	for target in "$d" "$(dirname "$d")"; do
		[ -f "$target/power/control" ] || continue
		before=$(cat "$target/power/control")
		delay=$(cat "$target/power/autosuspend_delay_ms" 2>/dev/null)
		echo "  $(basename "$target"): control=$before  autosuspend_delay_ms=${delay:-n/a}"
		if [ "$before" != "on" ]; then
			if echo on > "$target/power/control" 2>/dev/null; then
				echo "    -> set to $(cat "$target/power/control")"
			else
				echo "    -> could not write power/control"
			fi
		fi
	done
done

if [ "$found" = 0 ]; then
	echo "no /sys/block/sr* found - is the drive attached?"
	ls /dev/sr* 2>/dev/null || echo "no /dev/sr* either"
fi
