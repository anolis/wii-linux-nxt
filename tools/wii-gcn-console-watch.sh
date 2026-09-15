#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Run on the Wii, redirecting stdout/stderr to a regular file (not the console).
# Usage: wii-gcn-console-watch.sh [seconds=120] [pid=auto]
# Automatic mode captures D-state runner notices and other blocked tasks.
set -eu
seconds=${1:-120}
target=${2:-auto}
case "$seconds" in ''|*[!0-9]*) exit 2 ;; esac
[ "$seconds" -ge 1 ] && [ "$seconds" -le 3600 ] || exit 2
case "$target" in auto) ;; ''|*[!0-9]*) exit 2 ;; esac
if [ "$target" != auto ]; then
	[ "$target" -gt 0 ] || exit 2
fi
[ "$#" -le 2 ] || exit 2
end=$(( $(date +%s) + seconds ))
samples=0
printf 'console-watch: start seconds=%s target=%s uptime=' "$seconds" "$target"
cat /proc/uptime
capture()
{
	pid=$1
	[ -d "/proc/$pid" ] || return 0
	printf '\nconsole-watch: pid=%s uptime=' "$pid"
	cat /proc/uptime
	for field in comm status wchan stack; do
		printf '\n/proc/%s/%s:\n' "$pid" "$field"
		cat "/proc/$pid/$field" 2>&1 || true
	done
}
while [ "$(date +%s)" -lt "$end" ] && [ "$samples" -lt 3 ]; do
	match=
	if [ "$target" != auto ]; then
		[ -d "/proc/$target" ] || break
		match=$target
	else
		for candidate in $(ps -eo pid,stat,comm | awk '$2 ~ /^D/ && ($3 == "bash" || $3 == "sh") {print $1}'); do
			command=$(tr '\000' ' ' < "/proc/$candidate/cmdline" 2>/dev/null) || continue
			case "$command" in
			*'gcn-render-cycle:'*) match=$candidate; break ;;
			esac
		done
	fi
	if [ -n "$match" ]; then
		printf '\nconsole-watch: sample=%s notice_pid=%s\n' "$samples" "$match"
		capture "$match"
		# Include possible lock holders/workers without flooding the kernel log.
		ps -eo pid,stat,comm
		for blocked in $(ps -eo pid,stat,comm | awk '$2 ~ /^D/ {print $1}' | head -n 32); do
			[ "$blocked" = "$match" ] || capture "$blocked"
		done
		samples=$((samples + 1))
	fi
	sleep 2
done
printf '\nconsole-watch: done samples=%s uptime=' "$samples"
cat /proc/uptime
