#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# Runs once when the dev container is created (postCreateCommand).  Every
# step but the last is mandatory: a failure stops here with the failing
# command's own error, rather than being reported as missing test data.
set -euo pipefail
cd "$(dirname "$0")/.."

git submodule update --init --recursive
(cd tests/e2e && npm ci && npx --yes playwright install --with-deps chromium)
pre-commit install

# Optional: the proprietary test data needs GS_TEST_DATA_TOKEN.  The script
# explains what is missing and how to get it (docs/guide/TEST_DATA.md).
./scripts/fetch-test-data.sh || true
