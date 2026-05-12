#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOC_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${DOC_ROOT}/.." && pwd)"

DEFAULT_INPUT="${DOC_ROOT}/api/dtu_unified_maintenance_sdk_api_design.md"
INPUT_PATH="${1:-${DEFAULT_INPUT}}"

if [[ ! -f "${INPUT_PATH}" ]]; then
  echo "error: markdown file not found: ${INPUT_PATH}" >&2
  exit 1
fi

INPUT_ABS="$(cd "$(dirname "${INPUT_PATH}")" && pwd)/$(basename "${INPUT_PATH}")"
DEFAULT_INPUT_ABS="$(cd "$(dirname "${DEFAULT_INPUT}")" && pwd)/$(basename "${DEFAULT_INPUT}")"
DOC_BASENAME="$(basename "${INPUT_ABS}" .md)"
if [[ $# -ge 2 ]]; then
  OUTPUT_PATH="${2}"
elif [[ "${INPUT_ABS}" == "${DEFAULT_INPUT_ABS}" ]]; then
  OUTPUT_PATH="${DOC_ROOT}/generated/动态库协议接口.pdf"
else
  OUTPUT_PATH="${DOC_ROOT}/generated/${DOC_BASENAME}.pdf"
fi
OUTPUT_ABS="$(mkdir -p "$(dirname "${OUTPUT_PATH}")" && cd "$(dirname "${OUTPUT_PATH}")" && pwd)/$(basename "${OUTPUT_PATH}")"

BUILD_DIR="${DOC_ROOT}/tmp/build_pdf/${DOC_BASENAME}"
RENDERED_MD="${BUILD_DIR}/${DOC_BASENAME}.with-diagrams.md"
HTML_PATH="${BUILD_DIR}/${DOC_BASENAME}.html"
BASE_PDF="${BUILD_DIR}/${DOC_BASENAME}.base.pdf"
OUTLINED_PDF="${BUILD_DIR}/${DOC_BASENAME}.outlined.pdf"
CSS_PATH="${BUILD_DIR}/markdown-pdf.css"

mkdir -p "${BUILD_DIR}"
cp "${DOC_ROOT}/tools/pdf/markdown-pdf.css" "${CSS_PATH}"

for cmd in python3 node npm pandoc; do
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    echo "error: required command not found: ${cmd}" >&2
    exit 1
  fi
done

PDF_RENDERER_KIND=""
PDF_RENDERER_CMD=""
if command -v wkhtmltopdf >/dev/null 2>&1; then
  PDF_RENDERER_KIND="wkhtmltopdf"
  PDF_RENDERER_CMD="$(command -v wkhtmltopdf)"
else
  for candidate in \
    "msedge" \
    "microsoft-edge" \
    "google-chrome" \
    "chromium" \
    "/mnt/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe" \
    "/mnt/c/Program Files/Microsoft/Edge/Application/msedge.exe" \
    "/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe" \
    "/c/Program Files/Microsoft/Edge/Application/msedge.exe"
  do
    if command -v "${candidate}" >/dev/null 2>&1; then
      PDF_RENDERER_KIND="edge"
      PDF_RENDERER_CMD="$(command -v "${candidate}")"
      break
    elif [[ -x "${candidate}" ]]; then
      PDF_RENDERER_KIND="edge"
      PDF_RENDERER_CMD="${candidate}"
      break
    fi
  done
fi

if [[ -z "${PDF_RENDERER_CMD}" ]]; then
  echo "error: required command not found: wkhtmltopdf or Edge/Chrome headless renderer" >&2
  exit 1
fi

PYPDF_PYTHON=""
PYPDF_PYTHON_CANDIDATES=()
if [[ -n "${PDF_PYTHON:-}" ]]; then
  PYPDF_PYTHON_CANDIDATES+=("${PDF_PYTHON}")
fi
PYPDF_PYTHON_CANDIDATES+=(python3 python python.exe py.exe)

for candidate in "${PYPDF_PYTHON_CANDIDATES[@]}"; do
  if command -v "${candidate}" >/dev/null 2>&1; then
    resolved_candidate="$(command -v "${candidate}")"
  elif [[ -x "${candidate}" ]]; then
    resolved_candidate="${candidate}"
  else
    continue
  fi

  if "${resolved_candidate}" - <<'PY' >/dev/null 2>&1
import importlib.util
import sys

sys.exit(0 if importlib.util.find_spec("pypdf") else 1)
PY
  then
    PYPDF_PYTHON="${resolved_candidate}"
    break
  fi
done

if [[ -z "${PYPDF_PYTHON}" ]]; then
  echo "error: required Python package not found: pypdf" >&2
  echo "error: install it for a bash-visible Python, for example: python3 -m pip install -r doc/requirements-docs.txt" >&2
  exit 1
fi

native_path() {
  if command -v wslpath >/dev/null 2>&1; then
    wslpath -w "$1"
  else
    printf '%s\n' "$1"
  fi
}

file_url() {
  local native
  native="$(native_path "$1")"
  native="${native//\\//}"
  if [[ "${native}" =~ ^[A-Za-z]: ]]; then
    printf 'file:///%s\n' "${native}"
  else
    printf 'file://%s\n' "${native}"
  fi
}

is_windows_command() {
  [[ "$1" == *.exe || "$1" == *.EXE ]]
}

path_for_command() {
  local command_path="$1"
  local file_path="$2"
  if is_windows_command "${command_path}"; then
    native_path "${file_path}"
  else
    printf '%s\n' "${file_path}"
  fi
}

run_pypdf_python() {
  local script_path="$1"
  shift

  local args=()
  args+=("$(path_for_command "${PYPDF_PYTHON}" "${script_path}")")
  for arg in "$@"; do
    args+=("$(path_for_command "${PYPDF_PYTHON}" "${arg}")")
  done

  "${PYPDF_PYTHON}" "${args[@]}"
}

python3 "${DOC_ROOT}/scripts/render_mermaid_markdown.py" \
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
  --css "$(basename "${CSS_PATH}")" \
  --output "${HTML_PATH}"

if [[ "${PDF_RENDERER_KIND}" == "wkhtmltopdf" ]]; then
  "${PDF_RENDERER_CMD}" \
    --enable-local-file-access \
    --encoding utf-8 \
    "${HTML_PATH}" \
    "${BASE_PDF}"
else
  EDGE_PROFILE="${BUILD_DIR}/edge-profile"
  mkdir -p "${EDGE_PROFILE}"
  "${PDF_RENDERER_CMD}" \
    --headless \
    --disable-gpu \
    --allow-file-access-from-files \
    --user-data-dir="$(native_path "${EDGE_PROFILE}")" \
    --print-to-pdf="$(native_path "${BASE_PDF}")" \
    --no-pdf-header-footer \
    --print-to-pdf-no-header \
    "$(file_url "${HTML_PATH}")" \
    >/dev/null 2>&1
fi

run_pypdf_python "${DOC_ROOT}/scripts/add_pdf_outline.py" \
  "${INPUT_ABS}" \
  "${BASE_PDF}" \
  "${OUTLINED_PDF}"

run_pypdf_python "${DOC_ROOT}/scripts/validate_pdf_export.py" \
  "${OUTLINED_PDF}"

cp "${OUTLINED_PDF}" "${OUTPUT_ABS}"

echo "generated: ${OUTPUT_ABS}"
