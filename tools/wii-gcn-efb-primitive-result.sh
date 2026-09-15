#!/bin/sh
set -eu
requested=$(cat /sys/module/gcn_gx/parameters/efb_clear_iterations)
result=$(cat /sys/module/gcn_gx/parameters/efb_clear_result)
printf 'efb-clear-client: requested=%s result=%s\n' "$requested" "$result"
test "$requested" = "${1:-1000}"
test "$result" = 0
test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_test)" = Y
case "${2:-split}" in
cpu-write)
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_cpu_write)" = Y
	;;
single)
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_single_quad)" = Y
	;;
two)
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_two_quads)" = Y
	;;
four-rows)
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_four_rows)" = Y
	;;
interior-even)
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_interior_only)" = Y
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_interior_even)" = Y
	;;
interior)
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_odd_rows)" = Y
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_interior_only)" = Y
	;;
edge-pair)
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_odd_rows)" = Y
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_edge_pair)" = Y
	;;
odd-rows)
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_two_rows)" = Y
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_odd_rows)" = Y
	;;
alternate)
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_two_rows)" = Y
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_alternate)" = Y
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_interleave)" = N
	;;
interleaved)
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_two_rows)" = Y
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_interleave)" = Y
	;;
two-rows)
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_two_rows)" = Y
	;;
padded)
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_four_rows)" = N
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_two_rows)" = N
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_pad_bands)" = Y
	;;
repeat)
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_pad_bands)" = N
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_repeat_bands)" = Y
	;;
bands)
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_repeat_bands)" = N
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_bands)" = Y
	;;
full)
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_bands)" = N
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_full_width)" = Y
	;;
batch)
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_full_width)" = N
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_batch)" = Y
	;;
split)
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_batch)" = N
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_single_quad)" = N
	test "$(cat /sys/module/gcn_gx/parameters/efb_primitive_two_quads)" = N
	;;
*) exit 2 ;;
esac
printf 'efb-primitive-client: PASS\n'
