#!/usr/bin/env python3

from __future__ import annotations

import re
import sys
from pathlib import Path

from pypdf import PdfReader, PdfWriter


HEADING_RE = re.compile(r"^(#{1,3})\s+(.*\S)\s*$")


def normalize(text: str) -> str:
    return " ".join(text.split())


def parse_headings(markdown_path: Path) -> list[tuple[int, str]]:
    headings: list[tuple[int, str]] = []
    for line in markdown_path.read_text(encoding="utf-8").splitlines():
        match = HEADING_RE.match(line)
        if not match:
            continue
        headings.append((len(match.group(1)), match.group(2).strip()))
    return headings


def find_heading_pages(
    reader: PdfReader,
    headings: list[tuple[int, str]],
) -> list[tuple[int, str, int]]:
    page_texts = [normalize(page.extract_text() or "") for page in reader.pages]
    located: list[tuple[int, str, int]] = []
    search_start = 0

    for level, title in headings:
        target = normalize(title)
        found = None
        for idx in range(search_start, len(page_texts)):
            if target and target in page_texts[idx]:
                found = idx
                search_start = idx
                break
        if found is None:
            found = located[-1][2] if located else 0
        located.append((level, title, found))

    return located


def add_outline(writer: PdfWriter, located: list[tuple[int, str, int]]) -> None:
    parents: dict[int, object] = {}
    for level, title, page_index in located:
        parent = parents.get(level - 1)
        item = writer.add_outline_item(title=title, page_number=page_index, parent=parent)
        parents[level] = item
        for deeper in list(parents):
            if deeper > level:
                parents.pop(deeper, None)


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: add_pdf_outline.py <source.md> <input.pdf> <output.pdf>", file=sys.stderr)
        return 1

    markdown_path = Path(sys.argv[1])
    input_pdf = Path(sys.argv[2])
    output_pdf = Path(sys.argv[3])

    headings = parse_headings(markdown_path)
    reader = PdfReader(str(input_pdf))
    writer = PdfWriter()

    for page in reader.pages:
        writer.add_page(page)

    if reader.metadata:
        writer.add_metadata({k: str(v) for k, v in reader.metadata.items() if v is not None})

    add_outline(writer, find_heading_pages(reader, headings))

    with output_pdf.open("wb") as fp:
        writer.write(fp)

    print(f"wrote outlined pdf: {output_pdf}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

