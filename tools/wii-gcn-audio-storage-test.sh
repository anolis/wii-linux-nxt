#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Root-only, temporary request-limit experiment on the Wii root SD card.
set -eu
client=${1:?Usage: wii-gcn-audio-storage-test.sh CLIENT [BASELINE_KIB]}
queue=/sys/block/mmcblk0/queue/max_sectors_kb
original=$(cat "$queue")
baseline=${2:-$original}
case "$baseline" in 4|8|16|32|64|128|256|512) ;; *) exit 2 ;; esac
job=$(mktemp -d /var/tmp/wii-audio-storage.XXXXXX)
player=
cleanup() {
    if [ -n "$player" ]; then
        kill -TERM "$player" 2>/dev/null || true
        wait "$player" 2>/dev/null || true
    fi
    echo "$original" > "$queue"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
for limit in "$baseline" 4; do
    echo "$limit" > "$queue"
    cat "$queue" > "$job/$limit-limit.txt"
    runuser -u wii -- "$client" hw:WiiAI 48000 20 480 4 silence > "$job/$limit-audio.log" 2>&1 &
    player=$!
    sleep 1
    dd if=/dev/zero of="$job/test-data" bs=64K count=64 oflag=direct conv=fdatasync 2> "$job/$limit-write.log"
    dd if="$job/test-data" of=/dev/null bs=64K iflag=direct 2> "$job/$limit-read.log"
    sha256sum "$job/test-data" > "$job/$limit-sha256.txt"
    read -r digest remainder < "$job/$limit-sha256.txt"
    test "$digest" = bb9f8df61474d25e71fa00722318cd387396ca1736605e1248821cc0de3d3af8
    result=PASS
    wait "$player" || result=FAIL
    player=
    if [ "$limit" = 4 ]; then capped_result=$result; fi
    cat /proc/asound/card0/ai > "$job/$limit-driver.txt"
    printf 'Request cap %s KiB: audio %s\n' "$limit" "$result"
    cat "$job/$limit-audio.log" "$job/$limit-driver.txt" "$job/$limit-write.log" "$job/$limit-read.log" "$job/$limit-sha256.txt"
done
printf 'Evidence: %s (original limit restored on exit)\n' "$job"
test "$capped_result" = PASS
