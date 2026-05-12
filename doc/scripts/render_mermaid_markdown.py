#!/usr/bin/env python3

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


MERMAID_BLOCK_RE = re.compile(r"```mermaid\s*\n(.*?)\n```", re.DOTALL)
MARKDOWN_IMAGE_RE = re.compile(r"(!\[[^\]]*\]\()([^)\n]+)(\))")
URI_SCHEME_RE = re.compile(r"^[a-zA-Z][a-zA-Z0-9+.-]*:")


def split_markdown_destination(raw: str) -> tuple[str, str, bool]:
    stripped = raw.strip()
    if stripped.startswith("<"):
        close_index = stripped.find(">")
        if close_index != -1:
            return stripped[1:close_index], stripped[close_index + 1 :], True

    parts = stripped.split(maxsplit=1)
    if len(parts) == 1:
        return parts[0], "", False
    return parts[0], f" {parts[1]}", False


def should_rewrite_image_path(path: str) -> bool:
    return not (
        not path
        or path.startswith("#")
        or path.startswith("//")
        or URI_SCHEME_RE.match(path)
        or Path(path).is_absolute()
    )


def format_markdown_destination(path: str, suffix: str, was_wrapped: bool) -> str:
    if was_wrapped or any(ch.isspace() for ch in path) or "(" in path or ")" in path:
        return f"<{path}>{suffix}"
    return f"{path}{suffix}"


def rewrite_local_image_paths(content: str, input_dir: Path, output_dir: Path) -> str:
    def replacer(match: re.Match[str]) -> str:
        destination, suffix, was_wrapped = split_markdown_destination(match.group(2))
        if not should_rewrite_image_path(destination):
            return match.group(0)

        source_path = (input_dir / destination).resolve()
        rel_path = os.path.relpath(source_path, output_dir).replace(os.sep, "/")
        return (
            match.group(1)
            + format_markdown_destination(rel_path, suffix, was_wrapped)
            + match.group(3)
        )

    return MARKDOWN_IMAGE_RE.sub(replacer, content)


def find_npx_command() -> list[str]:
    if os.name == "nt":
        return [shutil.which("npx.cmd") or shutil.which("npx") or "npx"]

    npx = shutil.which("npx")
    if npx:
        return [npx]

    npx_cmd = shutil.which("npx.cmd")
    cmd_exe = shutil.which("cmd.exe")
    if npx_cmd and cmd_exe:
        return [cmd_exe, "/c", npx_cmd]

    return ["npx"]


def npx_needs_windows_paths(command: list[str]) -> bool:
    if os.name == "nt":
        return False

    command_text = " ".join(command).lower()
    return (
        "cmd.exe" in command_text
        or "npx.cmd" in command_text
        or command[0].lower().startswith("/mnt/")
    )


def path_for_npx(path: Path, use_windows_paths: bool) -> str:
    if not use_windows_paths:
        return str(path)
    if not shutil.which("wslpath"):
        return str(path)

    return subprocess.check_output(
        ["wslpath", "-w", str(path)],
        text=True,
    ).strip()


def find_browser_executable(use_windows_paths: bool) -> str | None:
    if os.name == "nt":
        candidates = [
            Path("C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"),
            Path("C:/Program Files/Microsoft/Edge/Application/msedge.exe"),
            Path("C:/Program Files/Google/Chrome/Application/chrome.exe"),
            Path("C:/Program Files (x86)/Google/Chrome/Application/chrome.exe"),
        ]
        for candidate in candidates:
            if candidate.exists():
                return str(candidate)

    if use_windows_paths:
        candidates = [
            Path("/mnt/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"),
            Path("/mnt/c/Program Files/Microsoft/Edge/Application/msedge.exe"),
            Path("/mnt/c/Program Files/Google/Chrome/Application/chrome.exe"),
            Path("/mnt/c/Program Files (x86)/Google/Chrome/Application/chrome.exe"),
        ]
        for candidate in candidates:
            if candidate.exists():
                return path_for_npx(candidate, True)

    for command in [
        "chromium",
        "chromium-browser",
        "google-chrome",
        "microsoft-edge",
        "msedge",
    ]:
        executable = shutil.which(command)
        if executable:
            return executable

    return None


def prepare_puppeteer_config(
    source_path: Path,
    build_dir: Path,
    use_windows_paths: bool,
) -> Path:
    config = json.loads(source_path.read_text(encoding="utf-8"))
    browser = find_browser_executable(use_windows_paths)
    if browser:
        config["executablePath"] = browser
    if config.get("headless") is True:
        config["headless"] = "new"
    if not browser and config.get("headless") != "new":
        return source_path

    output_path = build_dir / "puppeteer-config.json"
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        json.dumps(config, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return output_path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: render_mermaid_markdown.py <input.md> <output.md>", file=sys.stderr)
        return 1

    script_dir = Path(__file__).resolve().parent
    doc_root = script_dir.parent
    repo_root = doc_root.parent
    input_path = Path(sys.argv[1]).resolve()
    output_path = Path(sys.argv[2]).resolve()
    build_dir = output_path.parent
    work_dir = build_dir / "mermaid-src"
    asset_dir = build_dir / "mermaid-assets"
    css_path = doc_root / "tools/pdf/mermaid.css"
    puppeteer_cfg = doc_root / "tools/pdf/puppeteer-config.json"
    npx_cmd = find_npx_command()
    use_windows_npx_paths = npx_needs_windows_paths(npx_cmd)
    active_puppeteer_cfg = prepare_puppeteer_config(
        puppeteer_cfg,
        build_dir,
        use_windows_npx_paths,
    )

    build_dir.mkdir(parents=True, exist_ok=True)
    work_dir.mkdir(parents=True, exist_ok=True)
    asset_dir.mkdir(parents=True, exist_ok=True)

    content = rewrite_local_image_paths(
        input_path.read_text(encoding="utf-8"),
        input_path.parent,
        output_path.parent,
    )
    index = 0

    def replacer(match: re.Match[str]) -> str:
        nonlocal index
        index += 1

        block = match.group(1).strip() + "\n"
        source_path = work_dir / f"diagram-{index:02d}.mmd"
        asset_path = asset_dir / f"diagram-{index:02d}.svg"
        source_path.write_text(block, encoding="utf-8")

        cmd = [
            *npx_cmd,
            "-y",
            "@mermaid-js/mermaid-cli@10.4.0",
            "-q",
            "-p",
            path_for_npx(active_puppeteer_cfg, use_windows_npx_paths),
            "-C",
            path_for_npx(css_path, use_windows_npx_paths),
            "-t",
            "neutral",
            "-i",
            path_for_npx(source_path, use_windows_npx_paths),
            "-o",
            path_for_npx(asset_path, use_windows_npx_paths),
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
