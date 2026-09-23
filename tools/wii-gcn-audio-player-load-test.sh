#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Wii-only root wrapper; playback and CPU load run as the session user.
set -eu
stage=${1:?Usage: wii-gcn-audio-player-load-test.sh DRIVER_STAGE APP_FIXTURE_STAGE}
fixtures=${2:?Supply the Audio Player fixture stage}
worker=${WIIDESK_TEST_WORKER:-/usr/local/lib/wiidesk-session/wiidesk-audio-worker}
runs=${WIIDESK_AUDIO_LOAD_RUNS:-5}
case "$runs" in 0|1|2|3|4|5) ;; *) exit 2 ;; esac
queue=/sys/block/mmcblk0/queue/max_sectors_kb
original=$(cat "$queue")
job=$(mktemp -d /var/tmp/wii-audio-player-load.XXXXXX)
test "$(findmnt -n -o FSTYPE /run)" = tmpfs
logs=$(mktemp -d /run/wii-audio-player-load.XXXXXX)
cpu=
disk=
player=
cleanup() {
    for pid in "$player" "$cpu" "$disk"; do
        [ -n "$pid" ] || continue
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    echo "$original" > "$queue"
    # Persist evidence only after storage pressure is stopped.
    for file in "$logs"/*.log; do
        if [ -f "$file" ]; then cp "$file" "$job/"; fi
    done
}
trap cleanup EXIT
trap 'exit 130' INT TERM
echo 4 > "$queue"
runuser -u wii -- timeout 110 sha256sum /dev/zero > "$logs/cpu.log" 2>&1 & cpu=$!
timeout 110 sh -c 'while :; do
    dd if=/dev/zero of="$1" bs=64K count=64 oflag=direct conv=fdatasync || exit
    dd if="$1" of=/dev/null bs=64K iflag=direct || exit
done' sh "$job/test-data" > "$logs/disk.log" 2>&1 & disk=$!
failed=0
mkfifo "$logs/input"
n=0
while [ "$n" -lt "$runs" ]; do
    n=$((n + 1))
    verdict=PASS
    # Keep stdin open until natural completion, including slow process startup.
    exec 3<> "$logs/input"
    printf 'V 0\n' >&3
    timeout 20 runuser -u wii -- env WIIDESK_AUDIO_DIAGNOSTICS=1 "$worker" "$stage/control-tone.wav" default < "$logs/input" 3>&- > "$logs/player-$n.log" 2>&1 & player=$!
    wait "$player" || verdict=FAIL
    player=
    exec 3>&-
    grep -q 'STATE ended' "$logs/player-$n.log" || verdict=FAIL
    if grep -Eq 'XRUN|ERROR' "$logs/player-$n.log"; then verdict=FAIL; fi
    if [ "$verdict" = FAIL ]; then failed=$((failed + 1)); fi
    cat /proc/asound/card0/ai > "$logs/driver-$n.log"
    kill -0 "$cpu"
    kill -0 "$disk"
    printf 'Player load sweep: %s/%s complete, %s failures\n' "$n" "$runs" "$failed"
done
runuser -u wii -- env WIIDESK_AUDIO_TEST_TMPDIR=/dev/shm WIIDESK_TEST_BUILD="$(dirname "$worker")" WIIDESK_AUDIO_BACKEND_ONLY=1 WIIDESK_TEST_AUDIO_DEVICE=default sh "$stage/audio_target_smoke.sh" "$fixtures" > "$logs/controls.log" 2>&1 || failed=$((failed + 1))
cat "$logs/controls.log"
kill -0 "$cpu"
kill -0 "$disk"
printf 'Evidence: %s; failures=%s (original request limit restored on exit)\n' "$job" "$failed"
test "$failed" -eq 0
