#!/usr/bin/env python3

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path
from xml.etree import ElementTree as ET


W_NS = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
R_NS = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
WP_NS = "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"
A_NS = "http://schemas.openxmlformats.org/drawingml/2006/main"
PIC_NS = "http://schemas.openxmlformats.org/drawingml/2006/picture"

for prefix, uri in {
    "w": W_NS,
    "r": R_NS,
    "wp": WP_NS,
    "a": A_NS,
    "pic": PIC_NS,
}.items():
    ET.register_namespace(prefix, uri)


def w_tag(local: str) -> str:
    return f"{{{W_NS}}}{local}"


def w_attr(local: str) -> str:
    return f"{{{W_NS}}}{local}"


def first_child(parent: ET.Element, tag: str) -> ET.Element | None:
    for child in list(parent):
        if child.tag == tag:
            return child
    return None


def ensure_child(parent: ET.Element, tag: str, index: int | None = None) -> ET.Element:
    child = first_child(parent, tag)
    if child is not None:
        return child
    child = ET.Element(tag)
    if index is None:
        parent.append(child)
    else:
        parent.insert(index, child)
    return child


def remove_children(parent: ET.Element, tag: str) -> None:
    for child in list(parent):
        if child.tag == tag:
            parent.remove(child)


def set_w_attr(element: ET.Element, name: str, value: str | int) -> None:
    element.set(w_attr(name), str(value))


def cell_text(cell: ET.Element) -> str:
    return "".join(text.text or "" for text in cell.iter(w_tag("t"))).strip()


def table_rows(table: ET.Element) -> list[ET.Element]:
    return [child for child in list(table) if child.tag == w_tag("tr")]


def row_cells(row: ET.Element) -> list[ET.Element]:
    return [child for child in list(row) if child.tag == w_tag("tc")]


def table_header(table: ET.Element) -> list[str]:
    rows = table_rows(table)
    if not rows:
        return []
    return [cell_text(cell) for cell in row_cells(rows[0])]


def normalize_widths(widths: list[int], target: int) -> list[int]:
    total = sum(widths)
    if not widths or total == target:
        return widths
    scaled = [max(720, round(width * target / total)) for width in widths]
    scaled[-1] += target - sum(scaled)
    return scaled


def widths_for_table(header: list[str], col_count: int, body_width: int) -> list[int]:
    head = [item.replace(" ", "") for item in header]

    if col_count == 5 and head[:5] == ["API", "用途", "关键输入", "同步返回含义", "关联异步结果"]:
        return normalize_widths([3150, 1500, 1900, 2250, 1972], body_width)
    if col_count == 5 and head and head[0] == "回调字段":
        return normalize_widths([2100, 2350, 2600, 1900, 1822], body_width)
    if col_count == 5:
        return normalize_widths([2500, 2100, 2100, 2050, 2022], body_width)

    if col_count == 4 and head and head[0] == "动态库":
        return normalize_widths([2300, 3700, 1800, 2972], body_width)
    if col_count == 4 and head and head[0] == "回调":
        return normalize_widths([2350, 2850, 3650, 1922], body_width)
    if col_count == 4 and head and head[0] == "类型":
        return normalize_widths([2850, 1250, 2750, 3922], body_width)
    if col_count == 4 and head and head[0] == "命令类型":
        return normalize_widths([3100, 1950, 3600, 2122], body_width)
    if col_count == 4:
        return normalize_widths([2800, 2200, 2900, 2872], body_width)

    if col_count == 3 and head and head[0] in {"结构体", "类型"}:
        return normalize_widths([3100, 2200, 5472], body_width)
    if col_count == 3:
        return normalize_widths([3000, 3300, 4472], body_width)

    if col_count == 2:
        return normalize_widths([2200, body_width - 2200], body_width)

    return normalize_widths([body_width // max(1, col_count)] * max(1, col_count), body_width)


def set_table_property(table: ET.Element, name: str, attrs: dict[str, str | int]) -> ET.Element:
    tbl_pr = ensure_child(table, w_tag("tblPr"), 0)
    element = ensure_child(tbl_pr, w_tag(name))
    for key, value in attrs.items():
        set_w_attr(element, key, value)
    return element


def set_table_borders(table: ET.Element) -> None:
    tbl_pr = ensure_child(table, w_tag("tblPr"), 0)
    borders = ensure_child(tbl_pr, w_tag("tblBorders"))
    specs = {
        "top": ("single", 6, "808080"),
        "left": ("single", 4, "BFBFBF"),
        "bottom": ("single", 6, "808080"),
        "right": ("single", 4, "BFBFBF"),
        "insideH": ("single", 4, "D9D9D9"),
        "insideV": ("single", 4, "D9D9D9"),
    }
    for edge, (val, size, color) in specs.items():
        node = ensure_child(borders, w_tag(edge))
        set_w_attr(node, "val", val)
        set_w_attr(node, "sz", size)
        set_w_attr(node, "space", 0)
        set_w_attr(node, "color", color)


def set_table_cell_margins(table: ET.Element) -> None:
    tbl_pr = ensure_child(table, w_tag("tblPr"), 0)
    margins = ensure_child(tbl_pr, w_tag("tblCellMar"))
    for side, width in {"top": 70, "bottom": 70, "left": 90, "right": 90}.items():
        node = ensure_child(margins, w_tag(side))
        set_w_attr(node, "w", width)
        set_w_attr(node, "type", "dxa")


def set_cell_width(cell: ET.Element, width: int, no_wrap: bool) -> None:
    tc_pr = ensure_child(cell, w_tag("tcPr"), 0)
    tc_w = ensure_child(tc_pr, w_tag("tcW"), 0)
    set_w_attr(tc_w, "w", width)
    set_w_attr(tc_w, "type", "dxa")
    if no_wrap:
        ensure_child(tc_pr, w_tag("noWrap"))
    shd = first_child(tc_pr, w_tag("shd"))
    if shd is not None and not no_wrap:
        # Keep existing shading; this branch only avoids an unused local.
        pass


def shade_cell(cell: ET.Element, fill: str) -> None:
    tc_pr = ensure_child(cell, w_tag("tcPr"), 0)
    shd = ensure_child(tc_pr, w_tag("shd"))
    set_w_attr(shd, "val", "clear")
    set_w_attr(shd, "color", "auto")
    set_w_attr(shd, "fill", fill)


def set_run_format(run: ET.Element, size: int, bold: bool = False) -> None:
    r_pr = ensure_child(run, w_tag("rPr"), 0)
    style = first_child(r_pr, w_tag("rStyle"))
    style_value = style.get(w_attr("val")) if style is not None else ""

    fonts = ensure_child(r_pr, w_tag("rFonts"))
    if style_value == "VerbatimChar":
        font = "Consolas"
        set_w_attr(fonts, "ascii", font)
        set_w_attr(fonts, "hAnsi", font)
        set_w_attr(fonts, "cs", font)
        set_w_attr(fonts, "eastAsia", "Microsoft YaHei")
    else:
        set_w_attr(fonts, "ascii", "Arial")
        set_w_attr(fonts, "hAnsi", "Arial")
        set_w_attr(fonts, "eastAsia", "Microsoft YaHei")
        set_w_attr(fonts, "cs", "Arial")

    sz = ensure_child(r_pr, w_tag("sz"))
    sz_cs = ensure_child(r_pr, w_tag("szCs"))
    set_w_attr(sz, "val", size)
    set_w_attr(sz_cs, "val", size)

    if bold:
        ensure_child(r_pr, w_tag("b"))
        ensure_child(r_pr, w_tag("bCs"))


def set_paragraph_compact(paragraph: ET.Element) -> None:
    p_pr = ensure_child(paragraph, w_tag("pPr"), 0)
    spacing = ensure_child(p_pr, w_tag("spacing"))
    set_w_attr(spacing, "before", 0)
    set_w_attr(spacing, "after", 0)
    set_w_attr(spacing, "line", 240)
    set_w_attr(spacing, "lineRule", "auto")


def format_table(table: ET.Element, body_width: int) -> None:
    rows = table_rows(table)
    if not rows:
        return

    col_count = max((len(row_cells(row)) for row in rows), default=0)
    if col_count <= 0:
        return

    widths = widths_for_table(table_header(table), col_count, body_width)

    set_table_property(table, "tblW", {"w": body_width, "type": "dxa"})
    set_table_property(table, "tblLayout", {"type": "fixed"})
    set_table_borders(table)
    set_table_cell_margins(table)

    remove_children(table, w_tag("tblGrid"))
    tbl_grid = ET.Element(w_tag("tblGrid"))
    for width in widths:
        grid_col = ET.SubElement(tbl_grid, w_tag("gridCol"))
        set_w_attr(grid_col, "w", width)
    insert_at = 1 if first_child(table, w_tag("tblPr")) is not None else 0
    table.insert(insert_at, tbl_grid)

    if col_count >= 5:
        font_size = 13
    elif col_count == 4:
        font_size = 14
    elif len(rows) >= 12:
        font_size = 15
    else:
        font_size = 18

    for row_index, row in enumerate(rows):
        cells = row_cells(row)
        if row_index == 0:
            tr_pr = ensure_child(row, w_tag("trPr"), 0)
            ensure_child(tr_pr, w_tag("tblHeader"))
        for col_index, cell in enumerate(cells):
            width = widths[min(col_index, len(widths) - 1)]
            set_cell_width(cell, width, no_wrap=(col_index == 0))
            if row_index == 0:
                shade_cell(cell, "EAF2F8")
            for paragraph in cell.iter(w_tag("p")):
                set_paragraph_compact(paragraph)
            for run in cell.iter(w_tag("r")):
                set_run_format(run, font_size, bold=(row_index == 0))


def set_section_layout(root: ET.Element) -> None:
    for sect_pr in root.iter(w_tag("sectPr")):
        pg_sz = ensure_child(sect_pr, w_tag("pgSz"), 0)
        set_w_attr(pg_sz, "w", 11906)
        set_w_attr(pg_sz, "h", 16838)
        if w_attr("orient") in pg_sz.attrib:
            del pg_sz.attrib[w_attr("orient")]

        pg_mar = ensure_child(sect_pr, w_tag("pgMar"))
        for side in ("top", "right", "bottom", "left"):
            set_w_attr(pg_mar, side, 567)
        set_w_attr(pg_mar, "header", 284)
        set_w_attr(pg_mar, "footer", 284)
        set_w_attr(pg_mar, "gutter", 0)


def postprocess_docx(path: Path) -> int:
    with zipfile.ZipFile(path, "r") as source:
        entries = {name: source.read(name) for name in source.namelist()}

    document_name = "word/document.xml"
    root = ET.fromstring(entries[document_name])

    body_width = 11906 - 567 * 2
    set_section_layout(root)

    tables = list(root.iter(w_tag("tbl")))
    for table in tables:
        format_table(table, body_width)

    entries[document_name] = ET.tostring(root, encoding="utf-8", xml_declaration=True)

    tmp_path = path.with_suffix(path.suffix + ".tmp")
    with zipfile.ZipFile(tmp_path, "w", zipfile.ZIP_DEFLATED) as target:
        for name, data in entries.items():
            target.writestr(name, data)
    tmp_path.replace(path)
    return len(tables)


def first_heading(path: Path) -> str:
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("# "):
            return line[2:].strip()
    return path.stem


def run_command(command: list[str], cwd: Path) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    doc_root = script_dir.parent
    repo_root = doc_root.parent
    default_input = doc_root / "api" / "dtu_unified_maintenance_sdk_api_design.md"

    parser = argparse.ArgumentParser(description="Build a Word document with API-friendly table layout.")
    parser.add_argument("input", nargs="?", type=Path, default=default_input)
    parser.add_argument("output", nargs="?", type=Path)
    parser.add_argument("--skip-mermaid", action="store_true", help="Do not render Mermaid blocks before Pandoc.")
    args = parser.parse_args()

    input_path = args.input.resolve()
    if not input_path.is_file():
        print(f"error: markdown file not found: {input_path}", file=sys.stderr)
        return 1

    output_path = args.output
    if output_path is None:
        output_path = doc_root / "generated" / f"{input_path.stem}.docx"
    output_path = output_path.resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    build_dir = doc_root / "tmp" / "build_docx" / input_path.stem
    build_dir.mkdir(parents=True, exist_ok=True)

    pandoc_path = shutil.which("pandoc")
    if not pandoc_path:
        print("error: required command not found: pandoc", file=sys.stderr)
        return 1

    pandoc_input = input_path
    resource_paths = [input_path.parent, build_dir, repo_root]

    if not args.skip_mermaid:
        rendered_path = build_dir / f"{input_path.stem}.with-diagrams.md"
        render_script = doc_root / "scripts" / "render_mermaid_markdown.py"
        try:
            run_command([sys.executable, str(render_script), str(input_path), str(rendered_path)], repo_root)
        except subprocess.CalledProcessError:
            print("warning: Mermaid rendering failed; using source markdown for DOCX", file=sys.stderr)
        else:
            pandoc_input = rendered_path

    resource_path = ";".join(str(path) for path in resource_paths)
    run_command(
        [
            pandoc_path,
            str(pandoc_input),
            "--from=gfm",
            "--standalone",
            "--metadata",
            f"title={first_heading(input_path)}",
            "--resource-path",
            resource_path,
            "--output",
            str(output_path),
        ],
        repo_root,
    )

    table_count = postprocess_docx(output_path)
    print(f"generated: {output_path}")
    print(f"formatted tables: {table_count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
