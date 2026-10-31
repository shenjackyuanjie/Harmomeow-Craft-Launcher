#!/usr/bin/env bash
# 在 Linux/WSL 中执行；只生成本地测试产物，不调用 hdc、不安装依赖。
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "$0")/../../.." && pwd)"
BUILD="$ROOT/.test/vulkan-host"
: "${VULKAN_HEADERS:?请设置为包含 vulkan/vulkan.h 的官方 Vulkan 头文件目录}"
[[ -f "$VULKAN_HEADERS/vulkan/vulkan.h" ]] || { echo '未找到官方 Vulkan 头文件' >&2; exit 2; }
mkdir -p "$BUILD"
# -idirafter 保证宿主机 libc 优先，不让 OHOS SDK libc 头文件替代宿主机头文件。
"${CC:-cc}" -std=gnu11 -g -O1 -fno-omit-frame-pointer \
    -fsanitize=address,undefined -I "$ROOT/tools/vulkan/tests/stubs" \
    -idirafter "$VULKAN_HEADERS" \
    "$ROOT/tools/vulkan/tests/test_layout_tracking.c" -ldl -lpthread \
    -o "$BUILD/test_layout_tracking"
ASAN_OPTIONS=detect_leaks=1 "$BUILD/test_layout_tracking" "$@"
