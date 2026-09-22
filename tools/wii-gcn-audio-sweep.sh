#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Run as an ordinary audio-group user. All playback is silent.
set -eu
client=${1:?Usage: wii-gcn-audio-sweep.sh CLIENT READ_FIXTURE [RESULT_DIRECTORY]}
fixture=${2:?Supply an existing regular file for direct-read load}
result=${3:-$(mktemp -d /var/tmp/wii-audio-sweep.XXXXXX)}
mkdir -p "$result"
load=
cleanup() {
    if [ -n "$load" ]; then
        kill -TERM "$load" 2>/dev/null || true
        wait "$load" 2>/dev/null || true
        load=
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM
test -f "$fixture"
# Fail explicitly if this filesystem cannot support the intended disk load.
dd if="$fixture" of=/dev/null bs=64K iflag=direct status=none
total=12
count=0
failed=0
printf 'phase\trate\tperiod\tperiods\tresult\n' > "$result/results.tsv"
for phase in idle cpu disk; do
    case "$phase" in
        cpu) timeout 90 sha256sum /dev/zero > "$result/load-cpu.log" 2>&1 & load=$! ;;
        disk) timeout 90 sh -c 'while :; do dd if="$1" of=/dev/null bs=64K iflag=direct status=none || exit; done' sh "$fixture" > "$result/load-disk.log" 2>&1 & load=$! ;;
    esac
    for rate in 32000 48000; do
        for geometry in '480 4' '1024 8'; do
            set -- $geometry
            log="$result/$phase-$rate-$1-$2.log"
            verdict=PASS
            if ! "$client" hw:WiiAI "$rate" 8 "$1" "$2" silence > "$log" 2>&1; then
                verdict=FAIL
                failed=$((failed + 1))
            fi
            if [ -n "$load" ] && ! kill -0 "$load" 2>/dev/null; then
                printf 'Load generator exited unexpectedly\n' >&2
                exit 1
            fi
            count=$((count + 1))
            printf '%s\t%s\t%s\t%s\t%s\n' "$phase" "$rate" "$1" "$2" "$verdict" >> "$result/results.tsv"
            printf '\rAudio sweep: %d/%d complete, %d failures' "$count" "$total" "$failed"
        done
    done
    cleanup
done
printf '\nEvidence: %s\n' "$result"
test "$failed" -eq 0
