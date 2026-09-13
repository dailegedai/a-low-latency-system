#include "../include/SPSCRingBuffer.h"
#include "check.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

// TEST1: 单线程 FIFO 顺序
static void test_fifo()
{
    std::cout << "[fifo] single-thread FIFO order\n";
    SPSCRingBuffer<int> ring(8);
    for (int i = 0; i < 8; ++i) {
        CHECK(ring.tryPush(i));
    }
    CHECK(ring.full());
    CHECK(!ring.tryPush(99));
    for (int i = 0; i < 8; ++i) {
        int v = -1;
        CHECK(ring.tryPop(v));
        CHECK(v == i);
    }
    CHECK(ring.empty());
    std::cout << "  PASS\n";
}

// TEST2: 容量向上取 2 的幂
static void test_capacity_rounding()
{
    std::cout << "[capacity] round up to power of two\n";
    SPSCRingBuffer<int> ring(100);
    CHECK(ring.capacity() == 128);
    CHECK(ring.size() == 0);
    CHECK(ring.empty());
    std::cout << "  PASS\n";
}

// TEST3: 环绕复用（小容量、多次 push/pop）
static void test_wrap()
{
    std::cout << "[wrap] small capacity heavy reuse\n";
    SPSCRingBuffer<uint64_t> ring(4);
    uint64_t next_push = 0;
    uint64_t next_pop = 0;
    for (int round = 0; round < 100000; ++round) {
        // 塞到满
        while (ring.tryPush(next_push)) {
            ++next_push;
        }
        // 排到空并校验顺序
        uint64_t v = 0;
        while (ring.tryPop(v)) {
            CHECK(v == next_pop);
            ++next_pop;
        }
    }
    CHECK(next_push == next_pop);
    std::cout << "  PASS\n";
}

// TEST4: 批量 push/pop（含部分写入）
static void test_batch()
{
    std::cout << "[batch] push/pop batch\n";
    SPSCRingBuffer<int> ring(8);
    int in[8] = {10, 11, 12, 13, 14, 15, 16, 17};
    CHECK(ring.tryPushBatch(in, 8) == 8);
    CHECK(ring.full());
    CHECK(ring.tryPushBatch(in, 8) == 0);

    int out[8] = {0};
    CHECK(ring.tryPopBatch(out, 3) == 3);
    CHECK(out[0] == 10 && out[1] == 11 && out[2] == 12);
    CHECK(ring.size() == 5);

    int in2[4] = {20, 21, 22, 23};
    CHECK(ring.tryPushBatch(in2, 4) == 3); // 只剩 3 个空位

    CHECK(ring.tryPopBatch(out, 8) == 8);
    CHECK(out[0] == 13 && out[1] == 14 && out[2] == 15 && out[3] == 16 &&
          out[4] == 17 && out[5] == 20 && out[6] == 21 && out[7] == 22);
    CHECK(ring.empty());
    std::cout << "  PASS\n";
}

// TEST4b: 批量移动入队（move-only 类型）+ 部分入队语义
static void test_move_batch()
{
    std::cout << "[move batch] move-only elements + partial push\n";
    SPSCRingBuffer<std::unique_ptr<int>> ring(8);

    std::unique_ptr<int> in[3];
    in[0] = std::make_unique<int>(1);
    in[1] = std::make_unique<int>(2);
    in[2] = std::make_unique<int>(3);
    CHECK(ring.tryPushBatch(in, 3) == 3);
    // 成功入队的源元素变为 moved-from
    CHECK(in[0] == nullptr && in[1] == nullptr && in[2] == nullptr);
    CHECK(ring.size() == 3);

    std::unique_ptr<int> out[8];
    CHECK(ring.tryPopBatch(out, 8) == 3);
    CHECK(out[0] && *out[0] == 1);
    CHECK(out[1] && *out[1] == 2);
    CHECK(out[2] && *out[2] == 3);
    CHECK(ring.empty());

    // 先占 6 个槽，容量 8 只剩 2 个空位
    for (int i = 0; i < 6; ++i) {
        std::unique_ptr<int> u = std::make_unique<int>(i);
        CHECK(ring.tryPush(std::move(u)));
    }
    std::unique_ptr<int> more[3];
    more[0] = std::make_unique<int>(10);
    more[1] = std::make_unique<int>(11);
    more[2] = std::make_unique<int>(12);
    CHECK(ring.tryPushBatch(more, 3) == 2); // 只放得下 2 个
    CHECK(more[0] == nullptr && more[1] == nullptr);         // 已入队 -> moved-from
    CHECK(more[2] != nullptr && *more[2] == 12);             // 未入队 -> 保持原值
    std::cout << "  PASS\n";
}

// TEST5: 并发生产者/消费者，严格 FIFO + 无丢失无重复（校验和 + 顺序）
static void run_concurrent(size_t cap, uint64_t n, int id)
{
    SPSCRingBuffer<uint64_t> ring(cap);
    std::atomic<bool> ok{true};
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        for (uint64_t i = 0; i < n;) {
            if (ring.tryPush(i)) {
                ++i;
            } else {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    uint64_t expected = 0;
    uint64_t sum = 0;
    std::thread consumer([&] {
        while (expected < n) {
            uint64_t v = 0;
            if (ring.tryPop(v)) {
                if (v != expected) {
                    ok.store(false, std::memory_order_relaxed);
                }
                sum += v;
                ++expected;
            } else if (producer_done.load(std::memory_order_acquire) && ring.empty()) {
                break;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    const uint64_t expect_sum = n * (n - 1) / 2;
    CHECK(ok.load());
    CHECK(expected == n);
    CHECK(sum == expect_sum);
    std::cout << "  round " << id << " (cap=" << cap << ", n=" << n
              << "): FIFO/sum OK\n";
}

static void test_concurrent()
{
    std::cout << "[concurrent] producer/consumer, no loss/dup, strict FIFO\n";
    run_concurrent(4, 200000, 1);        // 小容量强制高频环绕
    run_concurrent(1024, 500000, 2);     // 中等容量
    const size_t big = llengine::nextPowerOfTwoChecked(65536);
    run_concurrent(big, 1000000, 3);     // 大容量吞吐
    std::cout << "  PASS\n";
}

int main()
{
    std::cout << "========== SPSCRingBuffer Test ==========\n\n";
    test_fifo();
    test_capacity_rounding();
    test_wrap();
    test_batch();
    test_move_batch();
    test_concurrent();
    std::cout << "\n========== ALL TESTS PASSED ==========\n";
    return 0;
}
