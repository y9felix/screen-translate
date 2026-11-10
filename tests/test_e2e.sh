#!/usr/bin/env bash
set -euo pipefail

test_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "FAIL: command $1 not found in PATH" >&2
        exit 1
    fi
}

test_cmd "screen-translate"
test_cmd "screen-translate-overlay"
test_cmd "screen-translate-render"
test_cmd "screen-translate-watcher"
test_cmd "grim"
test_cmd "wayland-scanner"

VENV_LENS="${HOME}/.local/share/screen-translate/venv/bin/lens_scan"
if [ ! -x "${VENV_LENS}" ]; then
    echo "FAIL: lens_scan not executable at ${VENV_LENS}" >&2
    exit 1
fi

"${VENV_LENS}" status >/dev/null 2>&1

echo "PASS: all components verified for end-to-end operation"
