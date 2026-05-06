#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DOC_ROOT="${REPO_ROOT}/doc"

DEFAULT_INPUT="${DOC_ROOT}/api/dtu_unified_maintenance_sdk_api_design.md"
INPUT_PATH="${1:-${DEFAULT_INPUT}}"

if [[ ! -f "${INPUT_PATH}" ]]; then
  echo "error: markdown file not found: ${INPUT_PATH}" >&2
  exit 1
fi

INPUT_ABS="$(cd "$(dirname "${INPUT_PATH}")" && pwd)/$(basename "${INPUT_PATH}")"
DOC_BASENAME="$(basename "${INPUT_ABS}" .md)"
OUTPUT_PATH="${2:-${DOC_ROOT}/generated/${DOC_BASENAME}.pdf}"
OUTPUT_ABS="$(mkdir -p "$(dirname "${OUTPUT_PATH}")" && cd "$(dirname "${OUTPUT_PATH}")" && pwd)/$(basename "${OUTPUT_PATH}")"

BUILD_DIR="${DOC_ROOT}/tmp/build_pdf/${DOC_BASENAME}"
RENDERED_MD="${BUILD_DIR}/${DOC_BASENAME}.with-diagrams.md"
HTML_PATH="${BUILD_DIR}/${DOC_BASENAME}.html"
BASE_PDF="${BUILD_DIR}/${DOC_BASENAME}.base.pdf"

mkdir -p "${BUILD_DIR}"

for cmd in python3 node npm pandoc wkhtmltopdf; do
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    echo "error: required command not found: ${cmd}" >&2
    exit 1
  fi
done

python3 "${REPO_ROOT}/scripts/render_mermaid_markdown.py" \
  "${INPUT_ABS}" \
  "${RENDERED_MD}"

DOC_TITLE="$(
python3 - "${INPUT_ABS}" <<'PY'
from pathlib import Path
import sys

for line in Path(sys.argv[1]).read_text(encoding="utf-8").splitlines():
    if line.startswith("# "):
        print(line[2:].strip())
        break
else:
    print(Path(sys.argv[1]).stem)
PY
)"

pandoc \
  "${RENDERED_MD}" \
  --from=gfm \
  --standalone \
  --metadata "title=${DOC_TITLE}" \
  --css "${REPO_ROOT}/tools/pdf/markdown-pdf.css" \
  --output "${HTML_PATH}"

wkhtmltopdf \
  --enable-local-file-access \
  --encoding utf-8 \
  "${HTML_PATH}" \
  "${BASE_PDF}"

if python3 - <<'PY' >/dev/null 2>&1
import importlib.util
import sys
sys.exit(0 if importlib.util.find_spec("pypdf") else 1)
PY
then
  python3 "${REPO_ROOT}/scripts/add_pdf_outline.py" \
    "${INPUT_ABS}" \
    "${BASE_PDF}" \
    "${OUTPUT_ABS}"
else
  cp "${BASE_PDF}" "${OUTPUT_ABS}"
  echo "warning: pypdf is not installed, skipped PDF outline generation" >&2
fi

echo "generated: ${OUTPUT_ABS}"
