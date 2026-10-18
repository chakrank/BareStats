#!/usr/bin/env bash
# BareStats Linux Test Runner Wrapper
# Automates test execution on Linux, invoking scripts/run_tests.py.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

if command -v python3 >/dev/null 2>&1; then
    PYTHON_CMD="python3"
elif command -v python >/dev/null 2>&1; then
    PYTHON_CMD="python"
else
    echo "Error: Python 3 is required to run the automated test suite." >&2
    exit 1
fi

exec "$PYTHON_CMD" "${REPO_ROOT}/scripts/run_tests.py" "$@"
