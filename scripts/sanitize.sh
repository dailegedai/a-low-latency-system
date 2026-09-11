#!/usr/bin/env bash
# Sanitizer 验证：ASAN+UBSAN 全量、TSAN 并发测试。
# 各自独立的 build 目录（.gitignore 已忽略 build-*/），不改动默认 Release 构建。
#
# 用法:
#   ./scripts/sanitize.sh            # 先 ASAN+UB，再 TSAN
#   ./scripts/sanitize.sh asan       # 只跑 ASAN+UB
#   ./scripts/sanitize.sh tsan       # 只跑 TSAN
#
# TSAN 常见坑（内核 >= 4.14 高熵 ASLR 会报 unexpected memory mapping）：
#   sysctl vm.mmap_rnd_bits=28   或   setarch "$(uname -m)" -R ./scripts/sanitize.sh tsan

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS="${JOBS:-$(nproc)}"
MODE="${1:-all}"

cd "$REPO_ROOT"

common_cmake_args=(
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
)

run_asan() {
    local dir="build-asan"
    echo "================ ASAN + UBSAN ================"
    cmake -S . -B "$dir" \
        "${common_cmake_args[@]}" \
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
    cmake --build "$dir" -j "$JOBS"
    echo "[asan] ctest ..."
    ASAN_OPTIONS="detect_leaks=1:abort_on_error=1" \
        UBSAN_OPTIONS="halt_on_error=1" \
        ctest --test-dir "$dir" --output-on-failure
}

run_tsan() {
    local dir="build-tsan"
    echo "================ TSAN ================"
    cmake -S . -B "$dir" \
        "${common_cmake_args[@]}" \
        -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -O1 -g" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
    cmake --build "$dir" -j "$JOBS"
    echo "[tsan] ctest ..."
    # 内核 >= 4.14 高熵 ASLR（vm.mmap_rnd_bits=32）会使 TSAN 启动即报
    # "unexpected memory mapping"。setarch -R 关闭该进程 ASLR 即可绕过，
    # 无需 root。若 setarch 不可用则直接跑（旧内核不受影响）。
    if command -v setarch >/dev/null 2>&1; then
        setarch "$(uname -m)" -R ctest --test-dir "$dir" --output-on-failure
    else
        ctest --test-dir "$dir" --output-on-failure
    fi
}

case "$MODE" in
    asan) run_asan ;;
    tsan) run_tsan ;;
    all)  run_asan && run_tsan ;;
    *) echo "usage: $0 [asan|tsan|all]" >&2; exit 1 ;;
esac

echo "[sanitize] $MODE DONE"
