// 分配计数基准：证明 fire-and-forget 提交路径消除了 packaged_task/shared_ptr 分配。
//
// 方法：替换全局 operator new/delete 做计数（确定性，与 CPU 频率/调度噪声无关，
// 正好补 benchmark_util 计时在噪声环境下的不足）。
//
// 说明：本执行文件刻意不注册进 ctest；sanitizer 构建下 ASAN/TSAN 会拦截分配器，
// 此时退化为无计数（只保证可编译），避免与 sanitizer 冲突。

#include "../include/ThreadPool.h"
#include "../include/WorkStealingThreadPool.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ALLOC_COUNT_DISABLED 1
#endif

#ifdef ALLOC_COUNT_DISABLED

int main()
{
    std::printf("[allocation_benchmark] sanitizer 构建下分配计数被禁用，跳过。\n");
    return 0;
}

#else // -------------------- 正常构建 --------------------

static std::atomic<long long> g_new_calls{0};
static std::atomic<long long> g_delete_calls{0};
static std::atomic<long long> g_new_bytes{0};

static void* count_alloc(std::size_t sz)
{
    g_new_calls.fetch_add(1, std::memory_order_relaxed);
    g_new_bytes.fetch_add(static_cast<long long>(sz), std::memory_order_relaxed);
    if (void *p = std::malloc(sz)) {
        return p;
    }
    throw std::bad_alloc();
}

static void count_free(void *p) noexcept
{
    if (p) {
        g_delete_calls.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void *operator new(std::size_t sz)
{
    return count_alloc(sz);
}
void *operator new[](std::size_t sz)
{
    return count_alloc(sz);
}
void operator delete(void *p) noexcept
{
    count_free(p);
}
void operator delete[](void *p) noexcept
{
    count_free(p);
}
void operator delete(void *p, std::size_t) noexcept
{
    count_free(p);
}
void operator delete[](void *p, std::size_t) noexcept
{
    count_free(p);
}

static void reset_counters()
{
    g_new_calls.store(0, std::memory_order_relaxed);
    g_delete_calls.store(0, std::memory_order_relaxed);
    g_new_bytes.store(0, std::memory_order_relaxed);
}

template <typename Fn>
static void measure(const char *label, int n, Fn &&fn)
{
    reset_counters();
    for (int i = 0; i < n; ++i) {
        fn(i);
    }
    const long long allocs = g_new_calls.load(std::memory_order_relaxed);
    const long long frees = g_delete_calls.load(std::memory_order_relaxed);
    const long long bytes = g_new_bytes.load(std::memory_order_relaxed);
    std::printf("%-42s allocs=%8lld (%6.3f/submit)  frees=%8lld  bytes=%lld\n",
                label, allocs, static_cast<double>(allocs) / n, frees, bytes);
}

int main()
{
    constexpr int N = 200000;
    const std::array<char, 128> big{};

    std::printf("=== Allocation per submit (N=%d) ===\n\n", N);

    // ThreadPool(0 workers, queue cap 1, DISCARD)：任务不执行、不阻塞，
    // 隔离出纯"提交路径"的分配次数。任何 make_shared/packaged_task 都会计入。
    {
        ThreadPool pool(0, 1, RejectPolicy::DISCARD);
        measure("ThreadPool submit(future) [empty]", N,
                [&](int) { pool.submit([] {}); });
    }
    {
        ThreadPool pool(0, 1, RejectPolicy::DISCARD);
        measure("ThreadPool submitVoid  [empty]", N,
                [&](int) { pool.submitVoid([] {}); });
    }
    {
        ThreadPool pool(0, 1, RejectPolicy::DISCARD);
        measure("ThreadPool submitVoid  [capture 128B]", N,
                [&](int) { pool.submitVoid([big] { (void)big[0]; }); });
    }

    // WorkStealingThreadPool：验证同一 Task 构造路径（1 worker 会 drain）。
    {
        WorkStealingThreadPool pool(1, 2, WorkStealingRejectPolicy::DISCARD);
        measure("WST submit(future) [empty]", N,
                [&](int) { pool.submit([] {}); });
    }
    {
        WorkStealingThreadPool pool(1, 2, WorkStealingRejectPolicy::DISCARD);
        measure("WST submitVoid  [empty]", N,
                [&](int) { pool.submitVoid([] {}); });
    }

    std::printf("\n注：ThreadPool 场景 0 worker + DISCARD 隔离纯提交路径；\n");
    std::printf("    submitVoid[empty] 的 allocs/submit 取决于闭包是否落在\n");
    std::printf("    std::function 的 SBO 内（libstdc++ 约 16B），见 phase2 文档。\n");
    return 0;
}

#endif
