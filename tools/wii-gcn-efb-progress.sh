#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Invoked on the Wii by wii-gcn-render-cycle.sh. Detailed output stays in dmesg.
set -eu
module=$1
requested=$2
shift 2
case "$requested" in ''|*[!0-9]*) exit 2 ;; esac
[ "$requested" -ge 1 ] && [ "$requested" -le 1000 ] || exit 2
show_progress()
{
	completed=$(cat /sys/module/gcn_gx/parameters/efb_clear_completed 2>/dev/null || echo 0)
	case "$completed" in ''|*[!0-9]*) completed=0 ;; esac
	[ "$completed" -le "$requested" ] || completed=$requested
	filled=$((completed * 30 / requested))
	bar=$(printf '%*s' "$filled" '' | tr ' ' '#')
	printf '\r[%-30s] %4s/%s iterations' "$bar" "$completed" "$requested"
	printf '\r[%-30s] %4s/%s iterations' "$bar" "$completed" "$requested" > /dev/tty0 2>/dev/null || true
}
insmod "$module" "$@" &
loader=$!
while kill -0 "$loader" 2>/dev/null; do
	show_progress
	sleep 1
done
status=0
wait "$loader" || status=$?
show_progress
printf '\n'
printf '\n' > /dev/tty0 2>/dev/null || true
exit "$status"
