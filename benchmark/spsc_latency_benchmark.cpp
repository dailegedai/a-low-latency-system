// SPSC 跨核基准 + 尾延迟归因（Phase 3）。
//
// 三种测量：
//   1) ping-pong 往返延迟：在途仅 1 条，测无积压握手延迟；
//      并把 RTT 拆成 forward(t1-t0) 与 back(t2-t1)，用于定位长尾发生在哪一半。
//   2) 同核对照：把生产者/消费者绑到**同一个 CPU**，若长尾由调度/上下文切换主导，
//      同核 RTT 会显著恶化（两线程争一个核）。
//   3) flood 吞吐：生产者灌满环、消费者批量排空，测 M msg/s。
//
// 归因手段（本机无 perf/CAP_SYS_NICE，见 docs/phase3）：
//   - RTT 相位拆分：长尾若集中在 back → 生产者被调离；集中在 forward → 消费者被调离。
//   - 每线程 voluntary / nonvoluntary 上下文切换计数（读 /proc/self/task/<tid>/status）。
//   - 同核 vs 跨核对照。

#include "../include/SPSCRingBuffer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>
#include <sys/syscall.h>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

using Clock = std::chrono::steady_clock;

static void spin_pause()
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

static bool pin_to_cpu(int cpu)
{
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)cpu;
    return false;
#endif
}

static std::vector<int> allowed_cpus()
{
    std::vector<int> v;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (int i = 0; i < CPU_SETSIZE; ++i) {
            if (CPU_ISSET(i, &set)) {
                v.push_back(i);
            }
        }
    }
#endif
    return v;
}

static uint64_t now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

struct CtxtSwitches {
    long voluntary = 0;
    long nonvoluntary = 0;
};

// 读取某 CPU 的中断+软中断累计计数（/proc/stat），用于估计运行期间的中断次数
static unsigned long long read_cpu_irq(int cpu)
{
#if defined(__linux__)
    if (cpu < 0) {
        return 0;
    }
    std::FILE* f = std::fopen("/proc/stat", "r");
    if (!f) {
        return 0;
    }
    char label[16];
    const int label_len = std::snprintf(label, sizeof(label), "cpu%d", cpu);
    char line[512];
    unsigned long long result = 0;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, label, static_cast<size_t>(label_len)) == 0 &&
            line[label_len] == ' ') {
            unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
            unsigned long long irq = 0, softirq = 0, steal = 0, guest = 0, guest_nice = 0;
            std::sscanf(line, "%*s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                        &user, &nice, &system, &idle, &iowait, &irq, &softirq,
                        &steal, &guest, &guest_nice);
            result = irq + softirq;
            break;
        }
    }
    std::fclose(f);
    return result;
#else
    (void)cpu;
    return 0;
#endif
}

// 读取本线程的上下文切换计数（Linux /proc）
static CtxtSwitches read_ctxt_switches()
{
    CtxtSwitches c;
#if defined(__linux__)
    const long tid = static_cast<long>(::syscall(SYS_gettid));
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%ld/status", tid);
    std::FILE* f = std::fopen(path, "r");
    if (!f) {
        return c;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        long v = 0;
        if (std::sscanf(line, "voluntary_ctxt_switches: %ld", &v) == 1) {
            c.voluntary = v;
        } else if (std::sscanf(line, "nonvoluntary_ctxt_switches: %ld", &v) == 1) {
            c.nonvoluntary = v;
        }
    }
    std::fclose(f);
#endif
    return c;
}

static uint64_t percentile(const std::vector<uint64_t>& v, double p)
{
    if (v.empty()) {
        return 0;
    }
    const size_t idx = static_cast<size_t>(p * static_cast<double>(v.size() - 1));
    return v[idx];
}

static void print_series(const char* label, std::vector<uint64_t> v)
{
    std::sort(v.begin(), v.end());
    uint64_t sum = 0;
    for (uint64_t x : v) {
        sum += x;
    }
    const double mean = v.empty() ? 0.0 : static_cast<double>(sum) / v.size();
    std::printf("  %s: mean=%.1f p50=%llu p90=%llu p99=%llu p999=%llu max=%llu (ns)\n",
                label, mean,
                (unsigned long long)percentile(v, 0.50),
                (unsigned long long)percentile(v, 0.90),
                (unsigned long long)percentile(v, 0.99),
                (unsigned long long)percentile(v, 0.999),
                (unsigned long long)(v.empty() ? 0 : v.back()));
}

struct Msg {
    uint64_t seq;
    uint64_t t0; // 生产者发出
    uint64_t t1; // 消费者收到
};

struct PingpongResult {
    std::vector<uint64_t> fwd; // t1 - t0
    std::vector<uint64_t> back; // t2 - t1
    std::vector<uint64_t> rtt;  // t2 - t0
    CtxtSwitches prod_sw;
    CtxtSwitches cons_sw;
    unsigned long long irq_prod = 0; // 运行期间生产者 CPU 的中断+软中断增量
    unsigned long long irq_cons = 0;
};

// 往返延迟：生产者发一条 → 消费者记录 t1 并回显 → 生产者测 RTT。在途恒为 1。
static PingpongResult run_pingpong(uint64_t n, uint64_t warmup,
                                   int cpu_prod, int cpu_cons)
{
    PingpongResult r;
    r.fwd.reserve(n);
    r.back.reserve(n);
    r.rtt.reserve(n);

    SPSCRingBuffer<Msg> req(1024);
    SPSCRingBuffer<Msg> resp(1024);
    std::atomic<bool> start{false};

    const unsigned long long irq_prod_before = read_cpu_irq(cpu_prod);
    const unsigned long long irq_cons_before = read_cpu_irq(cpu_cons);

    std::thread consumer([&] {
        if (cpu_cons >= 0) {
            pin_to_cpu(cpu_cons);
        }
        const CtxtSwitches before = read_ctxt_switches();
        while (!start.load(std::memory_order_acquire)) {
            spin_pause();
        }
        for (uint64_t i = 0; i < warmup + n; ++i) {
            Msg m;
            while (!req.tryPop(m)) {
                spin_pause();
            }
            m.t1 = now_ns();
            while (!resp.tryPush(m)) {
                spin_pause();
            }
        }
        const CtxtSwitches after = read_ctxt_switches();
        r.cons_sw.voluntary = after.voluntary - before.voluntary;
        r.cons_sw.nonvoluntary = after.nonvoluntary - before.nonvoluntary;
    });

    if (cpu_prod >= 0) {
        pin_to_cpu(cpu_prod);
    }
    const CtxtSwitches before = read_ctxt_switches();
    start.store(true, std::memory_order_release);
    for (uint64_t i = 0; i < warmup + n; ++i) {
        Msg m{i, now_ns(), 0};
        while (!req.tryPush(m)) {
            spin_pause();
        }
        Msg echo;
        while (!resp.tryPop(echo)) {
            spin_pause();
        }
        const uint64_t t2 = now_ns();
        if (i >= warmup) {
            r.fwd.push_back(echo.t1 - m.t0);
            r.back.push_back(t2 - echo.t1);
            r.rtt.push_back(t2 - m.t0);
        }
    }
    const CtxtSwitches after = read_ctxt_switches();
    r.prod_sw.voluntary = after.voluntary - before.voluntary;
    r.prod_sw.nonvoluntary = after.nonvoluntary - before.nonvoluntary;
    consumer.join();
    r.irq_prod = read_cpu_irq(cpu_prod) - irq_prod_before;
    r.irq_cons = read_cpu_irq(cpu_cons) - irq_cons_before;
    return r;
}

static void report_pingpong(const char* label, PingpongResult& r, uint64_t sample_limit)
{
    std::printf("\n--- %s: ping-pong (in-flight=1), samples=%zu ---\n", label, r.rtt.size());
    print_series("RTT  ", r.rtt);
    print_series("fwd  ", r.fwd);
    print_series("back ", r.back);

    // 长尾归因：统计超阈值样本，看其 forward/back 哪一半主导
    const uint64_t thresh = 5000; // 5µs
    size_t tail = 0;
    size_t fwd_dom = 0;
    size_t back_dom = 0;
    uint64_t max_fwd = 0;
    uint64_t max_back = 0;
    for (size_t i = 0; i < r.rtt.size(); ++i) {
        if (r.rtt[i] >= thresh) {
            ++tail;
            if (r.fwd[i] >= r.back[i]) {
                ++fwd_dom;
            } else {
                ++back_dom;
            }
        }
        max_fwd = std::max(max_fwd, r.fwd[i]);
        max_back = std::max(max_back, r.back[i]);
    }
    std::printf("  tail>=%lluns: %zu samples (fwd-dominant=%zu, back-dominant=%zu), "
                "max_fwd=%llu max_back=%llu\n",
                (unsigned long long)thresh, tail, fwd_dom, back_dom,
                (unsigned long long)max_fwd, (unsigned long long)max_back);
    std::printf("  ctxt-switches: producer vol=%ld nonvol=%ld | consumer vol=%ld nonvol=%ld\n",
                r.prod_sw.voluntary, r.prod_sw.nonvoluntary,
                r.cons_sw.voluntary, r.cons_sw.nonvoluntary);
    std::printf("  interrupts+softirq during run: producer_cpu=%llu consumer_cpu=%llu\n",
                r.irq_prod, r.irq_cons);

    // 打印最差的若干样本，给出相位拆分（归因证据）
    std::vector<size_t> idx(r.rtt.size());
    for (size_t i = 0; i < idx.size(); ++i) {
        idx[i] = i;
    }
    const size_t topk = std::min<size_t>(sample_limit, idx.size());
    std::partial_sort(idx.begin(), idx.begin() + topk, idx.end(),
                      [&](size_t a, size_t b) { return r.rtt[a] > r.rtt[b]; });
    std::printf("  worst %zu samples (seq, rtt, fwd, back):\n", topk);
    for (size_t k = 0; k < topk; ++k) {
        const size_t i = idx[k];
        std::printf("    #%zu rtt=%llu fwd=%llu back=%llu\n", i,
                    (unsigned long long)r.rtt[i],
                    (unsigned long long)r.fwd[i],
                    (unsigned long long)r.back[i]);
    }
}

// flood 吞吐
static double run_throughput(uint64_t n, int cpu_prod, int cpu_cons)
{
    SPSCRingBuffer<uint64_t> q(65536);
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};

    std::thread consumer([&] {
        if (cpu_cons >= 0) {
            pin_to_cpu(cpu_cons);
        }
        while (!start.load(std::memory_order_acquire)) {
            spin_pause();
        }
        uint64_t got = 0;
        uint64_t buf[256];
        while (got < n) {
            const size_t k = q.tryPopBatch(buf, 256);
            got += k;
            if (k == 0) {
                spin_pause();
            }
        }
        done.store(true, std::memory_order_release);
    });

    if (cpu_prod >= 0) {
        pin_to_cpu(cpu_prod);
    }
    const auto t0 = Clock::now();
    start.store(true, std::memory_order_release);
    for (uint64_t i = 0; i < n;) {
        if (q.tryPush(i)) {
            ++i;
        } else {
            spin_pause();
        }
    }
    while (!done.load(std::memory_order_acquire)) {
        spin_pause();
    }
    const auto t1 = Clock::now();
    consumer.join();

    const double sec = std::chrono::duration<double>(t1 - t0).count();
    return static_cast<double>(n) / sec / 1e6;
}

int main(int argc, char** argv)
{
    const uint64_t n = (argc > 1) ? static_cast<uint64_t>(std::strtoull(argv[1], nullptr, 10))
                                  : 200000ULL;
    const uint64_t warmup = 2000;

    const auto cpus = allowed_cpus();
    int cpu_a = cpus.empty() ? -1 : cpus[0];
    int cpu_b = (cpus.size() >= 2) ? cpus[1] : -1;

    std::printf("=== SPSC cross-core benchmark + tail attribution ===\n");
    std::printf("messages=%llu, warmup=%llu, allowed_cpus=%zu\n",
                (unsigned long long)n, (unsigned long long)warmup, cpus.size());
    std::printf("pinning: cross (prod=%d, cons=%d)\n", cpu_a, cpu_b);

    // 1) 跨核
    {
        PingpongResult cross = run_pingpong(n, warmup, cpu_a, cpu_b);
        report_pingpong("cross-core", cross, 5);
    }

    // 2) 同核对照（两线程争一个 CPU）：验证长尾的调度敏感性
    if (!cpus.empty()) {
        const uint64_t n_same = std::min<uint64_t>(n, 2000);
        const uint64_t w_same = std::min<uint64_t>(warmup, 100);
        std::printf("\npinning: same-core (both cpu=%d), messages=%llu\n",
                    cpu_a, (unsigned long long)n_same);
        PingpongResult same = run_pingpong(n_same, w_same, cpu_a, cpu_a);
        report_pingpong("same-core", same, 5);
    }

    // 3) flood 吞吐
    const double mmsg = run_throughput(n, cpu_a, cpu_b);
    std::printf("\n--- throughput (flood) ---\n");
    std::printf("messages=%llu, %.2f M msg/s\n", (unsigned long long)n, mmsg);

    return 0;
}
