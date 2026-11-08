#!/usr/bin/env bash
set -euo pipefail

BIN="/usr/local/bin/screen-translate"

if [ ! -x "${BIN}" ]; then
    echo "FAIL: ${BIN} missing or not executable" >&2
    exit 1
fi

output=$("${BIN}" --help 2>&1 || true)
if echo "${output}" | grep -q "screen-translate"; then
    echo "PASS: orchestrator executable and responds to --help"
    exit 0
else
    echo "FAIL: unexpected output: ${output}" >&2
    exit 1
fi
