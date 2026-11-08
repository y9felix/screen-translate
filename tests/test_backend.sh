#!/usr/bin/env bash
set -euo pipefail

VENV_DIR="${HOME}/.local/share/screen-translate/venv"
LENS_BIN="${VENV_DIR}/bin/lens_scan"

if [ ! -x "${LENS_BIN}" ]; then
    echo "FAIL: lens_scan missing or not executable at ${LENS_BIN}" >&2
    exit 1
fi

"${LENS_BIN}" status >/dev/null 2>&1
echo "PASS: lens_scan binary verified"
