#!/usr/bin/env python3

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


MERMAID_BLOCK_RE = re.compile(r"```mermaid\s*\n(.*?)\n```", re.DOTALL)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: render_mermaid_markdown.py <input.md> <output.md>", file=sys.stderr)
        return 1

    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parent
    input_path = Path(sys.argv[1]).resolve()
    output_path = Path(sys.argv[2]).resolve()
    build_dir = output_path.parent
    work_dir = build_dir / "mermaid-src"
    asset_dir = build_dir / "mermaid-assets"
    css_path = repo_root / "tools/pdf/mermaid.css"
    puppeteer_cfg = repo_root / "tools/pdf/puppeteer-config.json"

    build_dir.mkdir(parents=True, exist_ok=True)
    work_dir.mkdir(parents=True, exist_ok=True)
    asset_dir.mkdir(parents=True, exist_ok=True)

    content = input_path.read_text(encoding="utf-8")
    index = 0

    def replacer(match: re.Match[str]) -> str:
        nonlocal index
        index += 1

        block = match.group(1).strip() + "\n"
        source_path = work_dir / f"diagram-{index:02d}.mmd"
        asset_path = asset_dir / f"diagram-{index:02d}.svg"
        source_path.write_text(block, encoding="utf-8")

        cmd = [
            "npx",
            "-y",
            "@mermaid-js/mermaid-cli@10.4.0",
            "-q",
            "-p",
            str(puppeteer_cfg),
            "-C",
            str(css_path),
            "-t",
            "neutral",
            "-i",
            str(source_path),
            "-o",
            str(asset_path),
        ]
        subprocess.run(cmd, cwd=repo_root, check=True)

        rel_path = asset_path.relative_to(output_path.parent)
        return f"![Mermaid 图 {index}]({rel_path.as_posix()})"

    rendered = MERMAID_BLOCK_RE.sub(replacer, content)
    output_path.write_text(rendered, encoding="utf-8")
    print(f"rendered {index} mermaid diagrams")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
