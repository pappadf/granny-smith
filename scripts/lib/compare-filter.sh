# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# compare-filter.sh -- sourced by scripts/compare-integration-output.sh (the
# driver of compare-jobs-inline.sh and compare-output.sh): the one filter
# for integration output two runs may legitimately print differently.

# stdin -> stdout without the lines that differ between two runs of the
# corpus: the `# running...` heartbeat, the build stamp (the binary
# relinks), compile lines from a rebuild, the runner's wrapper label, tmp
# paths, wall-clock durations, and the daemon's PID and port.
compare_filter() {
    grep -v -E '^# running\.\.\.|^Granny Smith build|^gcc |^\[[0-9]+\] |^Daemon PID:' \
        | sed -E 's/ \(valgrind\)//; s#/tmp/[^ ]+#/tmp/X#g; s/[0-9]+(\.[0-9]+)? ?ms/N ms/g; s/(127\.0\.0\.1:|localhost )[0-9]+/\1PORT/g' || true
}
