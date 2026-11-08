#!/usr/bin/env bash
set -euo pipefail

DEST_DIR="${HOME}/.local/share/screen-translate/venv"
mkdir -p "$(dirname "${DEST_DIR}")"

if [ ! -d "${DEST_DIR}" ]; then
    python3 -m venv "${DEST_DIR}"
fi

"${DEST_DIR}/bin/pip" install --quiet --upgrade pip
"${DEST_DIR}/bin/pip" install --quiet "chrome-lens-py[clipboard,fonts]"
