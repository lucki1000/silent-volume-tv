#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
test_dir="$(mktemp -d "${TMPDIR:-/tmp}/silent-volume-tests.XXXXXX")"
trap 'rm -rf -- "$test_dir"' EXIT

g++ -std=c++11 -Wall -Wextra -Werror -pedantic \
    "$project_root/esp32-firmware/tests/hid_report_filter_test.cpp" \
    -o "$test_dir/hid-filter-test"
"$test_dir/hid-filter-test"

for test_file in "$project_root"/esp32-firmware/tests/*_test.py; do
    printf 'Running %s\n' "$(basename -- "$test_file")"
    PYTHONDONTWRITEBYTECODE=1 python3 "$test_file"
done
