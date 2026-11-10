#!/usr/bin/env bash
set -euo pipefail

PYTHON_BIN="${HOME}/.local/share/screen-translate/venv/bin/python"
RENDER_BIN="/usr/local/bin/screen-translate-render"

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

"${RENDER_BIN}" "${TEST_IN}" "${TEST_OUT}" >/dev/null 2>&1

if [ -f "${TEST_OUT}" ] && [ -s "${TEST_OUT}" ]; then
    echo "PASS: translation run produced output image"
    rm -f "${TEST_IN}" "${TEST_OUT}"
    exit 0
else
    echo "FAIL: screen-translate-render did not produce overlay image" >&2
    rm -f "${TEST_IN}" "${TEST_OUT}"
    exit 1
fi
