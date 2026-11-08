#!/usr/bin/env bash
set -euo pipefail

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    echo "usage: screen-translate [-s|--select]"
    exit 0
fi

LOCK_FILE="/run/user/${UID}/screen-translate.lock"
exec 200>"${LOCK_FILE}"
flock -n 200 || exit 0

ORIG_IMG="/dev/shm/screen-translate-orig-${UID}.png"
TRANS_IMG="/dev/shm/screen-translate-trans-${UID}.png"
VENV_LENS="${HOME}/.local/share/screen-translate/venv/bin/lens_scan"

OVERLAY_PID=""

cleanup() {
    rm -f "${ORIG_IMG}" "${TRANS_IMG}"
    if [ -n "${OVERLAY_PID}" ]; then
        kill "${OVERLAY_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

if [ "${1:-}" = "-s" ] || [ "${1:-}" = "--select" ]; then
    GEOM=$(slurp 2>/dev/null) || exit 0
    [ -n "${GEOM}" ] || exit 0
    grim -g "${GEOM}" "${ORIG_IMG}"
else
    grim "${ORIG_IMG}"
fi

chmod 600 "${ORIG_IMG}"

screen-translate-overlay "${ORIG_IMG}" "${TRANS_IMG}" &
OVERLAY_PID=$!

if "${VENV_LENS}" "${ORIG_IMG}" -t en --vertical-text horizontal -to "${TRANS_IMG}" >/dev/null 2>&1; then
    if [ -f "${TRANS_IMG}" ] && [ -s "${TRANS_IMG}" ]; then
        chmod 600 "${TRANS_IMG}" 2>/dev/null || true
        kill -SIGUSR1 "${OVERLAY_PID}" 2>/dev/null || true
    fi
    wait "${OVERLAY_PID}" 2>/dev/null || true
else
    wait "${OVERLAY_PID}" 2>/dev/null || true
fi
