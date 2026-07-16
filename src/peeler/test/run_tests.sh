#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# run_tests.sh — Test runner for libpeeler.
#
# Each test case is a sub-directory containing:
#   testfile.*   — the input archive
#   md5sums.txt  — expected checksums of extracted files
#
# The runner invokes the `peeler` CLI on the input, then validates the
# output with md5sum -c.
#
# Usage:
#   ./run_tests.sh                       Run all tests with auto-detected defaults
#   ./run_tests.sh --peeler <path> --test-dir <dir> [options]
#
# When invoked with no --peeler and no --test-dir, the script auto-detects:
#   peeler  → ../build/peeler  (relative to script directory)
#   test dirs → every directory in the script's directory that contains test cases
#
# Options:
#   --peeler <path>      Path to peeler executable
#   --test-dir <dir>     Directory containing test cases (may be repeated)
#   --output-dir <dir>   Temp directory for outputs (default: /tmp/peeler_test)
#   --verbose            Print peeler output on failure
#   --keep-files         Keep extracted files after testing
#   <name>               Run a single named test case

set -uo pipefail

# ============================================================================
# Resolve script directory (works even when called from another location)
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ============================================================================
# Defaults
# ============================================================================

PEELER=""
declare -a TEST_DIRS=()
OUTPUT_DIR="/tmp/peeler_test"
VERBOSE=false
KEEP_FILES=false
SINGLE_TEST=""

# ============================================================================
# Colors
# ============================================================================

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# ============================================================================
# Argument Parsing
# ============================================================================

usage() {
    cat <<EOF
Usage: $0 [options] [test-name]

When invoked with no --peeler / --test-dir, the script auto-detects:
  peeler   → ../build/peeler   (relative to script location)
  test dirs → all suitable directories next to the script

Options:
    --peeler <path>      Path to peeler executable
    --test-dir <dir>     Directory containing test cases (may be repeated)
    --output-dir <dir>   Temp directory for outputs (default: /tmp/peeler_test)
    --verbose            Print peeler output on failure
    --keep-files         Keep extracted files after testing
    <test-name>          Run only the named test case
EOF
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --peeler)    PEELER="$2";     shift 2 ;;
        --test-dir)  TEST_DIRS+=("$2"); shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --verbose)   VERBOSE=true;    shift ;;
        --keep-files) KEEP_FILES=true; shift ;;
        --help)      usage; exit 0 ;;
        --*)         echo "Unknown option: $1"; usage; exit 1 ;;
        *)
            if [[ -z "$SINGLE_TEST" ]]; then
                SINGLE_TEST="$1"; shift
            else
                echo "Unexpected argument: $1"; usage; exit 1
            fi
            ;;
    esac
done

# ============================================================================
# Auto-detect defaults when not supplied
# ============================================================================

if [[ -z "$PEELER" ]]; then
    PEELER="$SCRIPT_DIR/../build/peeler"
fi

if [[ ${#TEST_DIRS[@]} -eq 0 ]]; then
    # Auto-discover every directory next to the script that looks like a
    # test suite (contains at least one sub-dir with a testfile.* + md5sums.txt).
    for candidate in "$SCRIPT_DIR"/*/; do
        [[ -d "$candidate" ]] || continue
        for sub in "$candidate"/*/; do
            [[ -d "$sub" ]] || continue
            if [[ -f "$sub/md5sums.txt" ]]; then
                TEST_DIRS+=("$candidate")
                break
            fi
        done
    done
fi

# ============================================================================
# Validation
# ============================================================================

PEELER=$(realpath "$PEELER")

if [[ ! -x "$PEELER" ]]; then
    echo -e "${RED}Error: peeler not found or not executable: $PEELER${NC}" >&2
    exit 1
fi

if [[ ${#TEST_DIRS[@]} -eq 0 ]]; then
    echo -e "${RED}Error: no test directories found${NC}" >&2
    usage; exit 1
fi

for td in "${TEST_DIRS[@]}"; do
    td_resolved=$(realpath "$td")
    if [[ ! -d "$td_resolved" ]]; then
        echo -e "${RED}Error: test directory not found: $td_resolved${NC}" >&2
        exit 1
    fi
done

mkdir -p "$OUTPUT_DIR"

# ============================================================================
# Run all test suites
# ============================================================================

global_passed=0
global_failed=0
declare -a global_failed_tests=()

for TEST_DIR in "${TEST_DIRS[@]}"; do

TEST_DIR=$(realpath "$TEST_DIR")

# ============================================================================
# Discover Test Cases
# ============================================================================

# Label for the test suite based on directory name
suite_label=$(basename "$TEST_DIR")

declare -a test_cases=()
for dir in "$TEST_DIR"/*/; do
    [[ -d "$dir" ]] || continue
    name=$(basename "$dir")
    # Must contain at least one testfile.* and an md5sums.txt
    has_testfile=false
    for f in "$dir"/testfile.*; do
        [[ -f "$f" ]] && has_testfile=true && break
    done
    if $has_testfile && [[ -f "$dir/md5sums.txt" ]]; then
        test_cases+=("$name")
    fi
done

# Filter to single test if requested
if [[ -n "$SINGLE_TEST" ]]; then
    found=false
    for t in "${test_cases[@]}"; do
        if [[ "$t" == "$SINGLE_TEST" ]]; then
            test_cases=("$SINGLE_TEST")
            found=true
            break
        fi
    done
    if ! $found; then
        # This suite doesn't have the requested test — skip it silently
        continue
    fi
fi

if [[ ${#test_cases[@]} -eq 0 ]]; then
    echo -e "${YELLOW}No test cases found in $TEST_DIR${NC}"
    continue
fi

echo "[$suite_label] Running ${#test_cases[@]} test(s)..."

# ============================================================================
# Run Tests
# ============================================================================

passed=0
failed=0
declare -a failed_tests=()

for name in "${test_cases[@]}"; do
    test_src="$TEST_DIR/$name"
    test_out="$OUTPUT_DIR/$name"

    # Find the single testfile.*
    input_file=""
    for f in "$test_src"/testfile.*; do
        if [[ -f "$f" ]]; then
            if [[ -n "$input_file" ]]; then
                echo -e "${RED}  FAIL: $name — multiple testfiles${NC}"
                ((failed++))
                failed_tests+=("$name: multiple testfiles")
                continue 2
            fi
            input_file="$f"
        fi
    done
    if [[ -z "$input_file" ]]; then
        echo -e "${RED}  FAIL: $name — no testfile found${NC}"
        ((failed++))
        failed_tests+=("$name: no testfile")
        continue
    fi

    # Prepare clean output directory
    rm -rf "$test_out"
    mkdir -p "$test_out"

    # Run peeler
    if ! output=$("$PEELER" "$input_file" "$test_out" 2>&1); then
        echo -e "${RED}  FAIL: $name — peeler exited with error${NC}"
        if $VERBOSE; then
            echo "$output"
        fi
        ((failed++))
        failed_tests+=("$name: peeler error")
        continue
    fi

    # Validate checksums
    if (cd "$test_out" && md5sum -c "$test_src/md5sums.txt" >/dev/null 2>&1); then
        echo -e "${GREEN}  PASS: $name${NC}"
        ((passed++))
    else
        echo -e "${RED}  FAIL: $name — checksum mismatch${NC}"
        if $VERBOSE; then
            echo "  Expected:"
            grep -v '^#' "$test_src/md5sums.txt" | sed 's/^/    /'
            echo "  Actual:"
            (cd "$test_out" && find . -type f | sort | while read -r f; do
                md5=$(md5sum "$f" | awk '{print $1}')
                echo "    $md5  $f"
            done)
        fi
        ((failed++))
        failed_tests+=("$name: checksum mismatch")
    fi

    # Cleanup unless keeping files
    if ! $KEEP_FILES; then
        rm -rf "$test_out"
    fi
done

# ============================================================================
# Suite Summary
# ============================================================================

total=$((passed + failed))
echo
echo "[$suite_label] $passed/$total passed"

if [[ $failed -gt 0 ]]; then
    echo
    echo "Failed:"
    for f in "${failed_tests[@]}"; do
        echo "  $f"
    done
fi

global_passed=$((global_passed + passed))
global_failed=$((global_failed + failed))
global_failed_tests+=("${failed_tests[@]}")

done  # end for TEST_DIR

# ============================================================================
# Global Summary
# ============================================================================

if [[ ${#TEST_DIRS[@]} -gt 1 ]]; then
    global_total=$((global_passed + global_failed))
    echo
    echo "=============================="
    echo "Total: $global_passed/$global_total passed"
    if [[ $global_failed -gt 0 ]]; then
        echo
        echo "All failures:"
        for f in "${global_failed_tests[@]}"; do
            echo "  $f"
        done
    fi
fi

if [[ $global_failed -gt 0 ]]; then
    exit 1
fi

exit 0
