#!/bin/sh
# Run the host tests in $1 (make check)
cd "$(dirname "$0")" || exit 1
B=${1:-build}
fail=0
for t in test_endpoints test_settings test_info test_transfers test_midi_queue test_ump test_routing test_isoc_ring test_memory test_running_status test_stats test_latency; do
	"$B/$t" 2>/dev/null || fail=1
done
for g in trace layout; do
	if "$B/test_$g" 2>/dev/null | diff -u "golden/$g.txt" - > "$B/$g.diff"; then
		echo "$g: ok (matches golden/$g.txt)"
	else
		echo "$g: DIFFERS from golden/$g.txt (see $B/$g.diff; make golden if intended)"
		fail=1
	fi
done
[ $fail = 0 ] && echo "all tests passed" || echo "TESTS FAILED"
exit $fail
