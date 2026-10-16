#!/usr/bin/env bash
# This key is a recipe/source identity, not a promise that mutable apt mirrors reproduce bytes.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
{
    printf 'ubuntu-24.04-linux-amd64-sdk-v2-vvenc\n'
    git rev-parse HEAD:experiments/decoder-20261016/ffmpeg-7.0.3
    git rev-parse HEAD:third_party/vvenc
    sha256sum Dockerfile.ffmpeg scripts/build-ffmpeg.sh scripts/build-vvenc.sh scripts/archive-symbols.py
} | sha256sum | cut -c1-24
