#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# Compatibility entry point: one authoritative full suite selection.
exec bash "$root/scripts/test-service.sh" full "$@"
