#!/usr/bin/env bash
# This key is a recipe/source identity, not a promise that mutable apt mirrors reproduce bytes.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
python3 scripts/sdk-identity.py release-key
