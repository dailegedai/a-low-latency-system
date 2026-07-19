#include "../include/RingBuffer.h"

#include "benchmark_util.h"

#include <queue>
#include <cstdint>

// ── Scenario 1: Pre-allocated, reused buffer → zero alloc overhead ──
constexpr int CYCLE_CAP = 200000;
constexpr int CYCLES = 500;

static void bench_prealloc_cycles()
{
    {
        Benchmark b("std::queue (realloc cycles)");
        for (int c = 0; c < CYCLES; ++c) {
            std::queue<int> q;
            for (int i = 0; i < CYCLE_CAP; ++i) q.push(i);
            int x;
            while (!q.empty()) { x = q.front(); q.pop(); }
            (void)x;
        }
    }
    {
        Benchmark b("RingBuffer (zero-alloc cycles)");
        RingBuffer<int> q(CYCLE_CAP);
        for (int c = 0; c < CYCLES; ++c) {
            // Reuse same buffer — no allocation
            for (int i = 0; i < CYCLE_CAP; ++i) q.push(std::move(i));
            int x;
            while (!q.empty()) { q.pop(x); }
            (void)x;
        }
    }
}

// ── Scenario 2: High-frequency push/pop, small working set ──
constexpr int TOTAL_OPS = 50000000;

static void bench_high_throughput()
{
    {
        Benchmark b("std::queue (high throughput)");
        std::queue<int> q;
        int x;
        for (int i = 0; i < TOTAL_OPS; ++i) {
            q.push(i);
            x = q.front();
            q.pop();
        }
        (void)x;
    }
    {
        Benchmark b("RingBuffer (high throughput)");
        RingBuffer<int> q(64);
        int x;
        for (int i = 0; i < TOTAL_OPS; ++i) {
            q.push(std::move(i));
            q.pop(x);
        }
        (void)x;
    }
}

// ── Scenario 3: Medium payload, sequential access → cache locality ──
struct alignas(128) MidPayload {
    int id;
    char padding[124]; // 128 bytes total
};

static_assert(sizeof(MidPayload) == 128);

constexpr int MID_N = 500000;

static void bench_medium_payload()
{
    {
        Benchmark b("std::queue (128B payload)");
        std::queue<MidPayload> q;
        MidPayload p{};
        for (int i = 0; i < MID_N; ++i) { p.id = i; q.push(p); }
        MidPayload x;
        while (!q.empty()) { x = q.front(); q.pop(); }
        (void)x;
    }
    {
        Benchmark b("RingBuffer (128B payload)");
        RingBuffer<MidPayload> q(MID_N);
        MidPayload p{};
        for (int i = 0; i < MID_N; ++i) { p.id = i; q.push(std::move(p)); }
        MidPayload x;
        while (!q.empty()) { q.pop(x); }
        (void)x;
    }
}

int main()
{
    bench_high_throughput();
    std::cout << "---\n";
    bench_prealloc_cycles();
    std::cout << "---\n";
    bench_medium_payload();
    return 0;
}
