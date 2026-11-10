#!/usr/bin/env bash
set -euo pipefail

VENV_DIR="${HOME}/.local/share/screen-translate/venv"
LENS_BIN="${VENV_DIR}/bin/lens_scan"
PYTHON_BIN="${VENV_DIR}/bin/python"

TEST_IN="/tmp/test_lens_in.png"
TEST_OUT="/tmp/test_lens_out.png"

rm -f "${TEST_IN}" "${TEST_OUT}"

"${PYTHON_BIN}" - << 'EOF'
from PIL import Image, ImageDraw, ImageFont

img = Image.new("RGB", (600, 200), color=(255, 255, 255))
d = ImageDraw.Draw(img)
try:
    font = ImageFont.truetype("/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", 36, index=2)
except Exception:
    font = ImageFont.load_default()
d.text((50, 80), "日本語の設定画面", font=font, fill=(0, 0, 0))
img.save("/tmp/test_lens_in.png")
EOF

"${LENS_BIN}" "${TEST_IN}" -t en --manga --manga-growth 2.0 --min-text-size 16 -to "${TEST_OUT}" >/dev/null 2>&1

if [ -f "${TEST_OUT}" ] && [ -s "${TEST_OUT}" ]; then
    echo "PASS: translation run produced output image"
    rm -f "${TEST_IN}" "${TEST_OUT}"
    exit 0
else
    echo "FAIL: lens_scan did not produce overlay image" >&2
    rm -f "${TEST_IN}" "${TEST_OUT}"
    exit 1
fi
