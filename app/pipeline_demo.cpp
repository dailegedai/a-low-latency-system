// 用法: ./pipeline_demo [producers=4] [workers=4] [per_producer=200000] [paced_rate=500000]
//       paced_rate=0 表示只跑 saturated。

#include "../include/LockFreeQueue.h"
#include "../include/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static uint64_t now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

static void spin_pause()
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

static uint64_t percentile(const std::vector<uint64_t>& sorted, double p)
{
    if (sorted.empty()) {
        return 0;
    }
    const size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

struct PipeResult {
    uint64_t total = 0;
    uint64_t processed = 0;
    uint64_t checksum = 0;
    uint64_t expect_checksum = 0;
    uint64_t dropped = 0;
    uint64_t elapsed_ns = 0;
    std::vector<uint64_t> lat; // 未排序
};

// rate_per_sec <= 0 表示不限速（saturated）
static PipeResult run_pipeline(int producers, int workers, int per, double rate_per_sec)
{
    const uint64_t total = static_cast<uint64_t>(producers) * per;

    ThreadPool pool(workers, 65536);
    // 聚合出口：worker 把每条延迟采样 push 进 MPSC 无锁队列，主线程统一收集。
    LockFreeQueue<uint64_t> sink(1u << 20);

    std::atomic<uint64_t> processed{0};
    std::atomic<uint64_t> checksum{0};
    std::atomic<uint64_t> dropped{0};

    // 每生产者的目标周期（ns）；>0 表示限速
    const double per_prod_rate =
        (rate_per_sec > 0.0) ? (rate_per_sec / producers) : 0.0;
    const uint64_t period_ns =
        (per_prod_rate > 0.0) ? static_cast<uint64_t>(1e9 / per_prod_rate) : 0;

    const uint64_t t_start = now_ns();

    std::vector<std::thread> producer_threads;
    producer_threads.reserve(producers);
    for (int p = 0; p < producers; ++p) {
        producer_threads.emplace_back([&, p] {
            const uint64_t origin = now_ns();
            for (int i = 0; i < per; ++i) {
                if (period_ns > 0) {
                    // 绝对 deadline 防漂移；远时 sleep、近了 spin，兼顾精度与 CPU
                    const uint64_t deadline = origin + period_ns * static_cast<uint64_t>(i + 1);
                    for (;;) {
                        const uint64_t now = now_ns();
                        if (now >= deadline) {
                            break;
                        }
                        const uint64_t remain = deadline - now;
                        if (remain > 20000) {
                            std::this_thread::sleep_for(
                                std::chrono::nanoseconds(remain - 10000));
                        } else {
                            spin_pause();
                        }
                    }
                }
                const uint64_t seq = static_cast<uint64_t>(p) * per + i;
                const uint64_t t0 = now_ns();
                pool.submitVoid([&sink, &processed, &checksum, &dropped, seq, t0] {
                    // 极小的模拟工作，避免"空任务"测成纯调度
                    uint64_t x = seq * 2654435761u;
                    (void)x;

                    processed.fetch_add(1, std::memory_order_relaxed);
                    checksum.fetch_add(seq, std::memory_order_relaxed);

                    const uint64_t lat = now_ns() - t0;
                    if (!sink.push(uint64_t(lat))) {
                        dropped.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
        });
    }
    for (auto& th : producer_threads) {
        th.join();
    }

    // 等待全部任务完成（BLOCK 策略下 submitVoid 全部成功入队）
    while (pool.getCompletedTaskCount() < total) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    const uint64_t t_end = now_ns();

    PipeResult r;
    r.total = total;
    r.processed = processed.load();
    r.checksum = checksum.load();
    r.expect_checksum = total * (total - 1) / 2; // seq 恰为 0..total-1
    r.dropped = dropped.load();
    r.elapsed_ns = t_end - t_start;
    r.lat.reserve(total);
    uint64_t v = 0;
    while (sink.pop(v)) {
        r.lat.push_back(v);
    }
    std::sort(r.lat.begin(), r.lat.end());

    pool.shutdown();
    return r;
}

static void print_result(const char* label, const PipeResult& r)
{
    const double sec = static_cast<double>(r.elapsed_ns) / 1e9;
    const double throughput = static_cast<double>(r.total) / sec;
    uint64_t sum = 0;
    for (uint64_t x : r.lat) {
        sum += x;
    }
    const double mean = r.lat.empty() ? 0.0 : static_cast<double>(sum) / r.lat.size();

    std::printf("\n--- %s ---\n", label);
    std::printf("completed=%llu/%llu, elapsed=%.3f ms, %.0f tasks/s\n",
                (unsigned long long)r.processed, (unsigned long long)r.total,
                sec * 1e3, throughput);
    std::printf("latency submit->completion: samples=%zu dropped=%llu\n",
                r.lat.size(), (unsigned long long)r.dropped);
    std::printf("  mean=%.1f p50=%llu p90=%llu p99=%llu p999=%llu max=%llu (ns)\n",
                mean,
                (unsigned long long)percentile(r.lat, 0.50),
                (unsigned long long)percentile(r.lat, 0.90),
                (unsigned long long)percentile(r.lat, 0.99),
                (unsigned long long)percentile(r.lat, 0.999),
                (unsigned long long)(r.lat.empty() ? 0 : r.lat.back()));
    std::printf("checksum=%s\n",
                (r.checksum == r.expect_checksum) ? "OK" : "MISMATCH");
}

int main(int argc, char** argv)
{
    const int producers = (argc > 1) ? std::atoi(argv[1]) : 4;
    const int workers = (argc > 2) ? std::atoi(argv[2]) : 4;
    const int per = (argc > 3) ? std::atoi(argv[3]) : 200000;
    const double paced_rate = (argc > 4) ? std::atof(argv[4]) : 500000.0; // 0 = 只跑 saturated

    std::printf("=== pipeline demo ===\n");
    std::printf("producers=%d workers=%d per_producer=%d total=%llu paced_rate=%.0f/s\n",
                producers, workers, per,
                (unsigned long long)(static_cast<uint64_t>(producers) * per),
                paced_rate);

    PipeResult sat = run_pipeline(producers, workers, per, 0.0);
    print_result("saturated (no pacing): latency dominated by queueing", sat);

    if (paced_rate > 0.0) {
        PipeResult paced = run_pipeline(producers, workers, per, paced_rate);
        print_result("paced (rate-limited): queue near-empty", paced);

        const double sat_p50 = static_cast<double>(percentile(sat.lat, 0.50));
        const double paced_p50 = static_cast<double>(percentile(paced.lat, 0.50));
        std::printf("\n=== comparison (p50) ===\n");
        std::printf("saturated p50=%.0f ns  vs  paced p50=%.0f ns  ->  queueing %.1fx\n",
                    sat_p50, paced_p50,
                    (paced_p50 > 0.0) ? (sat_p50 / paced_p50) : 0.0);
    }

    return (sat.checksum == sat.expect_checksum) ? 0 : 1;
}
