# 基准测试方法论（Day31）

> 记录本项目基准测试的测量规范，使性能数据可复现、可比较。

## 核心原则

1. **warmup 预热**：正式采样前先跑若干轮，预热缓存 / TLB / 分支预测器，避免首轮冷缓存污染数据。
2. **重复采样**：同一测量多次运行，报告 `avg / min / max / median`。
3. **用 median 做结论**：均值对离群点（调度抢占、热降频、后台进程）敏感；中位数稳健。报告用 median。
4. **只做会话内对比**：跨会话的绝对值会因机器热状态 / 负载漂移，只有同一次会话内的 Before/After 或不同配置之间的相对比较才有意义。

## 实现：`benchmark_util.h`

```cpp
struct Stats {
    double avg_ms, min_ms, max_ms, median_ms;
};

// 预热 warmup 次，正式采样 repeats 次
template <typename Fn>
static Stats sample(const char* label, Fn&& fn, int warmup = 1, int repeats = 5);
```

输出形如：`label: avg=Xms min=Yms max=Zms median=Wms`。

## 采样规模建议

| 基准 | warmup | repeats |
|---|---|---|
| `false_sharing_benchmark` Part 2 | 1 | 3 |
| `throughput_benchmark`（每场景） | 1 | 3 |
| `submit_benchmark` / `benchmark` | 1 | 5 |

## 环境说明

- 本机 `cpupower` 存在但 **无 cpufreq 驱动**（`no or unknown cpufreq driver`），无法用 `cpupower frequency-set -g performance` 固定频率。
- 因此频率 / 热降频不可控：跨会话绝对值波动属预期，请勿直接比较不同日期的绝对数字。
- 采集时可选的噪声缓解（若环境支持）：固定 CPU 频率、`taskset` 绑定核、`nice -n -20`。

## 已知的噪声来源

1. CPU 频率缩放 / Turbo Boost 热降频
2. 操作系统调度、后台进程抢占
3. 缓存初始状态（冷/热）、TLB
4. 超线程 / NUMA 拓扑
5. 编译器优化程度（Release -O3 vs Debug）
