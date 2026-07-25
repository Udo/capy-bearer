#!/usr/bin/env bash
# Test-helper wrapper for the API coverage guard. Python is kept out of the
# build/engine path; this guard runs only from site/tests/api_coverage.uce.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 scripts/api_coverage_manifest.py
