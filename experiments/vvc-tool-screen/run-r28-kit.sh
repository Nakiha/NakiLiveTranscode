#!/bin/sh
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
libraries="$base/service/lib"
if [ "${NAKI_R28_NATIVE:-0}" = 1 ]; then
    /lib64/ld-linux-x86-64.so.2 --help | grep -q 'x86-64-v3 (supported' || {
        echo 'The optimized VVenC runtime requires x86-64-v3.' >&2
        exit 2
    }
    libraries="$base/native/lib:$libraries"
fi
export LD_LIBRARY_PATH="$libraries${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$base/service/bin/naki-live" "$@"
