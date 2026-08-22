#!/usr/bin/env bash
# Test-helper wrapper for the API coverage guard. Python is kept out of the
# build/engine path; this guard runs only from site/tests/api_coverage.capy.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 scripts/check_capy_doc_examples.py --self-test
exec python3 scripts/api_coverage_manifest.py
