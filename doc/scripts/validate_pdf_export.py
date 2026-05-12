#!/usr/bin/env python3

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Any

from pypdf import PdfReader


REJECTED_TEXT_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("browser file URL", re.compile(r"file://", re.IGNORECASE)),
    ("Windows absolute path", re.compile(r"\b[A-Za-z]:[\\/]", re.IGNORECASE)),
    ("WSL absolute path", re.compile(r"/mnt/[A-Za-z]/", re.IGNORECASE)),
    ("PDF build directory", re.compile(r"doc[/\\]tmp[/\\]build_pdf", re.IGNORECASE)),
)


def flatten_outline(items: list[Any]) -> list[Any]:
    entries: list[Any] = []
    for item in items:
        if isinstance(item, list):
            entries.extend(flatten_outline(item))
        else:
            entries.append(item)
    return entries


def validate_outline(reader: PdfReader) -> list[str]:
    errors: list[str] = []
    try:
        entries = flatten_outline(reader.outline)
    except Exception as exc:
        return [f"cannot read PDF outline: {type(exc).__name__}: {exc}"]

    if not entries:
        return ["PDF outline/bookmarks are missing"]

    page_count = len(reader.pages)
    for index, entry in enumerate(entries, start=1):
        title = getattr(entry, "title", f"outline item {index}")
        try:
            page_number = reader.get_destination_page_number(entry)
        except Exception as exc:
            errors.append(f"bookmark target cannot be resolved: {title} ({type(exc).__name__}: {exc})")
            continue
        if page_number < 0 or page_number >= page_count:
            errors.append(f"bookmark target out of range: {title} -> page index {page_number}")

    return errors


def validate_no_browser_footer(reader: PdfReader) -> list[str]:
    errors: list[str] = []
    page_count = len(reader.pages)

    for page_index, page in enumerate(reader.pages, start=1):
        text = page.extract_text() or ""
        for label, pattern in REJECTED_TEXT_PATTERNS:
            match = pattern.search(text)
            if match:
                errors.append(f"page {page_index}: found {label}: {match.group(0)}")

        page_counter = re.compile(rf"(?m)^\s*{page_index}\s*/\s*{page_count}\s*$")
        if page_counter.search(text):
            errors.append(f"page {page_index}: found browser page counter footer {page_index}/{page_count}")

    return errors


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: validate_pdf_export.py <pdf>", file=sys.stderr)
        return 1

    pdf_path = Path(sys.argv[1])
    reader = PdfReader(str(pdf_path))

    errors = validate_outline(reader)
    errors.extend(validate_no_browser_footer(reader))
    if errors:
        print(f"error: invalid PDF export: {pdf_path}", file=sys.stderr)
        for error in errors[:20]:
            print(f"- {error}", file=sys.stderr)
        if len(errors) > 20:
            print(f"- ... {len(errors) - 20} more", file=sys.stderr)
        return 1

    print(f"validated PDF export: {pdf_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
