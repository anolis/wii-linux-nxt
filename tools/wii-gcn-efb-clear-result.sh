#!/bin/sh
set -eu
requested=$(cat /sys/module/gcn_gx/parameters/efb_clear_iterations)
result=$(cat /sys/module/gcn_gx/parameters/efb_clear_result)
printf 'efb-clear-client: requested=%s result=%s\n' "$requested" "$result"
test "$requested" = "${1:-1000}"
test "$result" = 0
printf 'efb-clear-client: PASS\n'
