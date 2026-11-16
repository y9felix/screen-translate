#!/usr/bin/env bash
set -euo pipefail

if [ "${1:-}" = "-v" ] || [ "${1:-}" = "--version" ]; then
    echo "screen-translate 1.0.2"
    exit 0
fi

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    echo "usage: screen-translate [-s|--select] [-m|--mode]"
    exit 0
fi

PID_FILE="/run/user/${UID}/screen-translate-mode.pid"
OVERLAY_PID_FILE="/run/user/${UID}/screen-translate-overlay.pid"

if [ "${1:-}" = "-m" ] || [ "${1:-}" = "--mode" ]; then
    if [ -f "${PID_FILE}" ] && kill -0 "$(<"${PID_FILE}")" 2>/dev/null; then
        kill "$(<"${PID_FILE}")" 2>/dev/null || true
        rm -f "${PID_FILE}"
        pkill -TERM screen-translate-overlay 2>/dev/null || true
        osd -n "screen-translate" "Translation Mode: OFF" 2>/dev/null || true
    else
        screen-translate-watcher &
        echo $! > "${PID_FILE}"
        osd -n "screen-translate" "Translation Mode: ON" 2>/dev/null || true
    fi
    exit 0
fi

LOCK_FILE="/run/user/${UID}/screen-translate.lock"
exec 200>"${LOCK_FILE}"
flock 200

RUNNER_PID_FILE="/run/user/${UID}/screen-translate-runner.pid"
if [ -f "${RUNNER_PID_FILE}" ]; then
    OLD_PID=$(<"${RUNNER_PID_FILE}")
    if [ -n "${OLD_PID}" ] && [ "${OLD_PID}" != "$$" ] && kill -0 "${OLD_PID}" 2>/dev/null; then
        pkill -P "${OLD_PID}" 2>/dev/null || true
        kill "${OLD_PID}" 2>/dev/null || true
    fi
fi
echo $$ > "${RUNNER_PID_FILE}"
flock -u 200

ORIG_IMG="/dev/shm/screen-translate-orig-${UID}.png"
TRANS_IMG="/dev/shm/screen-translate-trans-${UID}.png"

OVERLAY_PID=""
RENDER_PID=""

cleanup() {
    rm -f "${ORIG_IMG}" "${TRANS_IMG}" "${OVERLAY_PID_FILE}"
    if [ -f "${RUNNER_PID_FILE}" ] && [ "$(<"${RUNNER_PID_FILE}")" = "$$" ]; then
        rm -f "${RUNNER_PID_FILE}"
    fi
    if [ -n "${RENDER_PID}" ]; then
        kill "${RENDER_PID}" 2>/dev/null || true
    fi
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
echo "${OVERLAY_PID}" > "${OVERLAY_PID_FILE}"

screen-translate-render "${ORIG_IMG}" "${TRANS_IMG}" >/dev/null 2>&1 &
RENDER_PID=$!

while kill -0 "${OVERLAY_PID}" 2>/dev/null && kill -0 "${RENDER_PID}" 2>/dev/null; do
    sleep 0.05
done

if ! kill -0 "${OVERLAY_PID}" 2>/dev/null; then
    kill "${RENDER_PID}" 2>/dev/null || true
else
    wait "${RENDER_PID}" 2>/dev/null || true
    if [ -f "${TRANS_IMG}" ] && [ -s "${TRANS_IMG}" ]; then
        chmod 600 "${TRANS_IMG}" 2>/dev/null || true
        kill -SIGUSR1 "${OVERLAY_PID}" 2>/dev/null || true
    fi
    wait "${OVERLAY_PID}" 2>/dev/null || true
fi
