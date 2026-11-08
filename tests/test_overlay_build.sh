#!/usr/bin/env bash
set -euo pipefail

OVERLAY_BIN="/usr/local/bin/screen-translate-overlay"

if [ ! -x "${OVERLAY_BIN}" ]; then
    echo "FAIL: overlay binary missing or not executable at ${OVERLAY_BIN}" >&2
    exit 1
fi

output=$("${OVERLAY_BIN}" -h 2>&1 || true)
if echo "${output}" | grep -q "usage:"; then
    echo "PASS: overlay binary exists and prints usage"
    exit 0
else
    echo "FAIL: unexpected output: ${output}" >&2
    exit 1
fi
