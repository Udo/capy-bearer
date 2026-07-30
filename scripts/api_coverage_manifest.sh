#!/usr/bin/env bash
# Test-helper wrapper for the API coverage guard. Python is kept out of the
# build/engine path; this guard runs only from site/tests/api_coverage.uce.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 scripts/check_capy_doc_examples.py --self-test
# The legacy corpus is still being converted. Enable the strict final gate with:
# python3 scripts/check_capy_doc_examples.py
exec python3 scripts/api_coverage_manifest.py
