#!/home/felix/.local/share/screen-translate/venv/bin/python
import asyncio
import math
import sys
from typing import List, Tuple
from PIL import Image, ImageFilter, ImageDraw
from chrome_lens_py.api import LensAPI
from chrome_lens_py.core.text_renderer import (
    _Scene, _box_corners, _convex_hull, composite, shape_for_display,
    should_reflow_horizontally, _render_reflow_paragraph, _render_line,
    _paragraph_alignment, _TRANSLATION_STATUS_SUCCESS, _WRITING_DIRECTION_TOP_TO_BOTTOM
)

def blur_hull_area(canvas: Image.Image, lines: list, padding: float) -> None:
    width, height = canvas.size
    points: List[Tuple[float, float]] = []
    thinnest = float("inf")
    for line in lines:
        if not line.HasField("geometry"):
            continue
        box = line.geometry.bounding_box
        points.extend(_box_corners(box, width, height))
        thinnest = min(thinnest, box.width * width, box.height * height)
    hull = _convex_hull(points)
    if len(hull) < 3:
        return
    pad = int(round(thinnest * padding)) if thinnest != float("inf") else 0
    margin = pad + 2
    x0 = max(0, math.floor(min(x for x, _ in hull)) - margin)
    y0 = max(0, math.floor(min(y for _, y in hull)) - margin)
    x1 = min(width, math.ceil(max(x for x, _ in hull)) + margin)
    y1 = min(height, math.ceil(max(y for _, y in hull)) + margin)
    if x1 <= x0 or y1 <= y0:
        return
    local = [(x - x0, y - y0) for x, y in hull]

    region = canvas.crop((x0, y0, x1, y1))
    radius = max(8, int(thinnest * 0.4))
    blurred = region.filter(ImageFilter.GaussianBlur(radius=radius))
    mask = Image.new("L", (x1 - x0, y1 - y0), 0)
    dmask = ImageDraw.Draw(mask)
    dmask.polygon(local, fill=255)
    if pad > 0:
        dmask.line(local + [local[0]], fill=255, width=pad * 2, joint="curve")
        for x, y in local:
            dmask.ellipse((x - pad, y - pad, x + pad, y + pad), fill=255)
    region.paste(blurred, mask=mask)
    canvas.paste(region, (x0, y0))

class SyntheticBox:
    def __init__(self, cx: float, cy: float, w: float, h: float, rot: float = 0.0):
        self.center_x = cx
        self.center_y = cy
        self.width = w
        self.height = h
        self.rotation_z = rot

class SyntheticGeometry:
    def __init__(self, bbox: SyntheticBox):
        self.bounding_box = bbox

class SyntheticParagraph:
    def __init__(self, p_list: list, g_list: list):
        combined = list(zip(p_list, g_list))
        combined.sort(key=lambda item: item[0].geometry.bounding_box.center_x, reverse=True)
        self.lines = []
        for p, _ in combined:
            self.lines.extend(p.lines)
        min_x = min(p.geometry.bounding_box.center_x - p.geometry.bounding_box.width / 2 for p, _ in combined)
        max_x = max(p.geometry.bounding_box.center_x + p.geometry.bounding_box.width / 2 for p, _ in combined)
        min_y = min(p.geometry.bounding_box.center_y - p.geometry.bounding_box.height / 2 for p, _ in combined)
        max_y = max(p.geometry.bounding_box.center_y + p.geometry.bounding_box.height / 2 for p, _ in combined)
        self.geometry = SyntheticGeometry(SyntheticBox(
            (min_x + max_x) / 2,
            (min_y + max_y) / 2,
            max_x - min_x,
            max_y - min_y
        ))

    def HasField(self, field: str) -> bool:
        return field == "geometry"

class SyntheticData:
    def __init__(self, g_list: list):
        combined = list(g_list)
        texts = [g.translation.translation.strip() for g in combined if g.translation.translation.strip()]
        self.translation = " ".join(texts)
        self.target_language = combined[0].translation.target_language
        self.writing_direction = combined[0].translation.writing_direction
        self.alignment = getattr(combined[0].translation, "alignment", 0)
        self.line = []
        for g in combined:
            self.line.extend(g.translation.line)

async def translate_image(in_path: str, out_path: str) -> int:
    api = LensAPI()
    img = Image.open(in_path).convert("RGBA")
    w, h = img.size
    res = await api.process_image(img, target_translation_language="en", include_raw_response=True)
    resp = res.get("raw_response_objects")
    if not resp:
        return 1

    paragraphs = list(resp.text.text_layout.paragraphs)
    gleams = list(resp.deep_gleams)
    if not paragraphs or not gleams:
        return 1

    vert_indices = []
    for i, p in enumerate(paragraphs):
        if i < len(gleams) and gleams[i].HasField("translation"):
            data = gleams[i].translation
            if data.status.code == _TRANSLATION_STATUS_SUCCESS and data.writing_direction == _WRITING_DIRECTION_TOP_TO_BOTTOM:
                vert_indices.append(i)

    parent = {i: i for i in vert_indices}
    def find(i: int) -> int:
        if parent[i] == i:
            return i
        parent[i] = find(parent[i])
        return parent[i]

    def union(i: int, j: int) -> None:
        parent[find(i)] = find(j)

    for i in vert_indices:
        for j in vert_indices:
            if i >= j:
                continue
            pi, pj = paragraphs[i], paragraphs[j]
            bi, bj = pi.geometry.bounding_box, pj.geometry.bounding_box
            y0_i, y1_i = (bi.center_y - bi.height / 2) * h, (bi.center_y + bi.height / 2) * h
            y0_j, y1_j = (bj.center_y - bj.height / 2) * h, (bj.center_y + bj.height / 2) * h
            ov_y = max(0, min(y1_i, y1_j) - max(y0_i, y0_j))
            min_h = min(bi.height * h, bj.height * h)
            if min_h > 0 and (ov_y / min_h) > 0.3:
                x0_i, x1_i = (bi.center_x - bi.width / 2) * w, (bi.center_x + bi.width / 2) * w
                x0_j, x1_j = (bj.center_x - bj.width / 2) * w, (bj.center_x + bj.width / 2) * w
                gap_x = max(0, max(x0_i, x0_j) - min(x1_i, x1_j))
                max_w = max(bi.width * w, bj.width * w)
                if gap_x < max_w * 2.5:
                    union(i, j)

    clusters = {}
    for i in vert_indices:
        clusters.setdefault(find(i), []).append(i)

    used = set()
    render_items = []
    for root, members in clusters.items():
        if len(members) > 1:
            p_list = [paragraphs[m] for m in members]
            g_list = [gleams[m] for m in members]
            render_items.append((SyntheticParagraph(p_list, g_list), SyntheticData(g_list)))
            used.update(members)
        else:
            m = members[0]
            render_items.append((paragraphs[m], gleams[m].translation))
            used.add(m)

    for i, p in enumerate(paragraphs):
        if i in used or i >= len(gleams) or not gleams[i].HasField("translation"):
            continue
        data = gleams[i].translation
        if data.status.code == _TRANSLATION_STATUS_SUCCESS:
            render_items.append((p, data))

    if not render_items:
        return 1

    scene = _Scene(
        canvas=img,
        font_path=None,
        draw_background=True,
        vertical_text="horizontal",
        erase_mode="blur",
        hull_padding=0.45,
        outline_scale=1.0,
        min_readable_px=16.0,
        text_align="auto",
        manga_mode=True,
        manga_box_growth=2.0
    )

    for p, d in render_items:
        blur_hull_area(scene.canvas, p.lines, scene.hull_padding)

    drawn = 0
    for p, d in render_items:
        vertical = d.writing_direction == _WRITING_DIRECTION_TOP_TO_BOTTOM
        is_rtl = (
            d.writing_direction == 1
            or d.target_language.split("-")[0].lower() in ("ar", "fa", "he", "ur")
        )
        align = _paragraph_alignment(scene, d, is_rtl)
        reflow = vertical and should_reflow_horizontally(scene.vertical_text, d.target_language)
        if reflow and p.HasField("geometry"):
            drawn += _render_reflow_paragraph(scene, p, d, align, is_rtl, use_hull=True)
        else:
            for l_idx in range(len(d.line)):
                if _render_line(scene, p, d, l_idx, vertical, is_rtl, align, use_hull=True):
                    drawn += 1

    if drawn == 0:
        return 1

    scene.canvas.save(out_path)
    return 0

def main() -> int:
    if len(sys.argv) < 3:
        sys.stderr.write("usage: screen-translate-render <in.png> <out.png>\n")
        return 1
    return asyncio.run(translate_image(sys.argv[1], sys.argv[2]))

if __name__ == "__main__":
    sys.exit(main())
