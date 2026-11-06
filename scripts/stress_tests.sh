#!/usr/bin/env bash
# 并发测试防回归：反复运行，直到出现一次失败或达到 REPEAT 次全过。
# 用于检测时序/竞态类偶发失败（flakiness）。
#
# 用法:
#   ./scripts/stress_tests.sh              # 默认 REPEAT=20
#   REPEAT=100 ./scripts/stress_tests.sh   # 跑 100 次全过
#
# 说明：仅默认 Release 档；sanitizer 下的重复运行请先跑 scripts/sanitize.sh。

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
REPEAT="${REPEAT:-20}"

cd "$REPO_ROOT"

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    echo "[stress] configuring $BUILD_DIR ..."
    cmake -S . -B "$BUILD_DIR"
fi
cmake --build "$BUILD_DIR" -j "$(nproc)"

# 并发/时序敏感的测试集合（不跑纯功能与基准）
TESTS_RE='bounded_queue_test|reject_policy_test|metrics_test|shutdown_test|memory_pool_test|lock_free_queue_test|locked_ring_buffer_test|work_stealing_deque_test|work_stealing_queue_test|light_epoch_test|work_stealing_threadpool_test|no_future_submit_test|spsc_ring_buffer_test|task_trace_test'

echo "[stress] repeat until-fail=${REPEAT} on concurrency tests ..."

# TSAN 构建：内核高熵 ASLR 会让 TSAN 启动即报 unexpected memory mapping，
# 需 setarch -R 关闭该进程 ASLR（与 scripts/sanitize.sh 一致）。
ctest_cmd=(ctest)
if [[ "$BUILD_DIR" == *tsan* ]] && command -v setarch >/dev/null 2>&1; then
    ctest_cmd=(setarch "$(uname -m)" -R ctest)
fi

"${ctest_cmd[@]}" --test-dir "$BUILD_DIR" --output-on-failure -R "$TESTS_RE" \
      --repeat until-fail:"$REPEAT"

echo "[stress] ALL GREEN (${REPEAT}x)"
