#!/usr/bin/env python3

from __future__ import annotations

import re
import sys
import unicodedata
from pathlib import Path

from pypdf import PdfReader, PdfWriter


HEADING_RE = re.compile(r"^(#{1,4})\s+(.*\S)\s*$")


def normalize(text: str) -> str:
    text = unicodedata.normalize("NFKC", text)
    text = re.sub(r"[^\w\u4e00-\u9fff]+", " ", text)
    return " ".join(text.split())


def parse_headings(markdown_path: Path) -> list[tuple[int, str, str]]:
    headings: list[tuple[int, str, str]] = []
    current_level: int | None = None
    current_title: str | None = None
    current_preview: list[str] = []

    def flush() -> None:
        nonlocal current_level, current_title, current_preview
        if current_level is None or current_title is None:
            return
        headings.append((current_level, current_title, " ".join(current_preview)))
        current_level = None
        current_title = None
        current_preview = []

    for line in markdown_path.read_text(encoding="utf-8").splitlines():
        match = HEADING_RE.match(line)
        if match:
            flush()
            current_level = len(match.group(1))
            current_title = match.group(2).strip()
            continue

        if current_level is None:
            continue

        stripped = line.strip()
        if not stripped or stripped.startswith("```"):
            continue
        if len(" ".join(current_preview)) >= 80:
            continue
        current_preview.append(stripped)

    flush()
    return headings


def find_heading_pages(
    reader: PdfReader,
    headings: list[tuple[int, str, str]],
) -> list[tuple[int, str, int]]:
    page_texts = [normalize(page.extract_text() or "") for page in reader.pages]
    located: list[tuple[int, str, int]] = []
    search_start = find_content_start(page_texts, headings)

    for level, title, _preview in headings:
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


def find_content_start(
    page_texts: list[str],
    headings: list[tuple[int, str, str]],
) -> int:
    earliest: int | None = None

    for _level, title, preview in headings:
        if not preview:
            continue

        combined = normalize(f"{title} {preview}")
        preview_only = normalize(preview)

        for candidate in iter_match_candidates(combined):
            for idx, page_text in enumerate(page_texts):
                if candidate in page_text:
                    earliest = idx if earliest is None else min(earliest, idx)
                    break
            if earliest == 0:
                return 0

        for candidate in iter_match_candidates(preview_only):
            for idx, page_text in enumerate(page_texts):
                if candidate in page_text:
                    earliest = idx if earliest is None else min(earliest, idx)
                    break
            if earliest == 0:
                return 0

    return earliest if earliest is not None else 0


def iter_match_candidates(text: str) -> list[str]:
    if not text:
        return []

    candidates: list[str] = []
    for length in (len(text), 80, 48, 32):
        candidate = text[:length].strip()
        if len(candidate) >= 12 and candidate not in candidates:
            candidates.append(candidate)
    return candidates


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
