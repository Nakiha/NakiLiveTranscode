#!/usr/bin/env bash
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
mode=${1:-phase2}
export PKG_CONFIG_PATH="$root/out/install/$mode/lib/pkgconfig"
clang -std=c11 -O2 -Wall -Wextra -INakiLiveTranscode/app -INakiLiveTranscode/third_party/yyjson/src -I"out/install/$mode/include" \
 research/phase2/config_switch_test.c research/phase2/config_identity_adapter.c \
 NakiLiveTranscode/app/control/config.c NakiLiveTranscode/app/control/frame_policy.c NakiLiveTranscode/app/control/frame_policy_config.c \
 NakiLiveTranscode/app/control/watermark_config.c NakiLiveTranscode/app/control/output_config.c NakiLiveTranscode/app/control/sei_config.c \
 NakiLiveTranscode/app/control/intent.c NakiLiveTranscode/app/control/control_spec.c NakiLiveTranscode/third_party/yyjson/src/yyjson.c \
 $(pkg-config --libs libavformat libavcodec libavutil) -lm -o "out/phase2/config-switch-$mode"
"out/phase2/config-switch-$mode"
