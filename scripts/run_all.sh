#!/usr/bin/env bash
# 一键验证：配置(如缺) → 构建全目标 → ctest 全量。
# 可选：在命令后追加 benchmark 目标名做冒烟（默认不跑 benchmark）。
#
# 用法:
#   ./scripts/run_all.sh                 # build + ctest
#   ./scripts/run_all.sh submit_benchmark  # build + ctest + 跑 submit_benchmark
#
# 在仓库根目录或任意子目录均可调用（自动定位根目录）。

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
JOBS="${JOBS:-$(nproc)}"

cd "$REPO_ROOT"

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    echo "[run_all] configuring $BUILD_DIR ..."
    cmake -S . -B "$BUILD_DIR"
fi

echo "[run_all] building (jobs=$JOBS) ..."
cmake --build "$BUILD_DIR" -j "$JOBS"

echo "[run_all] ctest ..."
ctest --test-dir "$BUILD_DIR" --output-on-failure

if [[ $# -gt 0 ]]; then
    for target in "$@"; do
        echo "[run_all] benchmark smoke: $target"
        "$BUILD_DIR/$target"
    done
fi

echo "[run_all] ALL GREEN"
