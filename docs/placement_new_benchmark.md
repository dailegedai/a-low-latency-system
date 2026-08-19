# MemoryPool 重构对比：`new Task()` vs Placement New

> 日期：2026-08-05
> 目标：验证 Placement New 重构对 MemoryPool 的影响
> 测试：`./build/memory_pool_test`（Release，-O3）

## 背景

### 状态 A（重构前）

`MemoryPool` 构造时用 N 次 `new Task()` 预分配对象，对象散落在堆上不同位置。

```cpp
MemoryPool::MemoryPool(size_t capacity) {
    pool_.reserve(capacity);
    for (size_t i = 0; i < capacity; i++)
        pool_.push_back(new Task());   // N 次 malloc
}
```

### 状态 B（重构后）

`MemoryPool` 用 `::operator new` 一次性分配连续原始内存，再用 placement new 预构造对象。

```cpp
MemoryPool::MemoryPool(size_t capacity)
    : capacity_(capacity), storage_(::operator new(capacity * sizeof(Task)))
{
    free_.reserve(capacity);
    auto* p = static_cast<char*>(storage_);
    for (size_t i = 0; i < capacity; ++i)
        free_.push_back(new (p + i * sizeof(Task)) Task());  // 1 次 malloc
}

MemoryPool::~MemoryPool() {
    for (Task* t : free_) t->~Task();   // 显式析构
    ::operator delete(storage_);
}
```

## 测量数据

### 单线程延迟（纳秒，1M 样本）

| 指标 | A（`new Task()`） | B（placement new） | 变化 |
|---|---|---|---|
| pool acquire avg | 24.56 ns | 24.93 ns | 持平（+1.5%） |
| pool acquire p50 | 20 ns | 20 ns | 持平 |
| pool acquire p99 | 31 ns | 31 ns | 持平 |
| raw new/delete avg | 54.28 ns | 50.83 ns | 对照基准 |
| speed-up avg | 2.21x | 2.04x | 持平 |
| speed-up p99 | 2.26x | 2.26x | 持平 |

> `min`/`max` 受调度和页错误影响，不稳定，不作比较。`max` 有 ms 级离群值（~0.4–0.6ms）。

### 单线程吞吐量（ops/sec，5M ops）

| 实现 | ops/sec |
|---|---|
| A pool | 75.5M |
| B pool | 73.3M |
| raw new/delete | 29–30M |

### 竞争池（共享 + 内部 mutex，0.5M ops/thread）

| 线程数 | A (总 / 每线程) | B (总 / 每线程) |
|---|---|---|
| 2 | 21.8M / 10.9M | 23.5M / 11.7M |
| 4 | 37.5M / 9.4M | 34.0M / 8.5M |
| 8 | 31.0M / 3.9M | 28.7M / 3.6M |

### 线程本地池（无竞争，1M ops/thread）

| 线程数 | A (总 / 每线程) | B (总 / 每线程) |
|---|---|---|
| 1 | 79.4M / 79.4M | 75.1M / 75.1M |
| 2 | 135.8M / 67.9M | 111.6M / 55.8M |
| 4 | 288.1M / 72.0M | 278.3M / 69.6M |
| 8 | 311.2M / 38.9M | 404.1M / 50.5M |

## 结论

### 1. 功能正确性

两种状态全部功能测试通过（含 zero-capacity、线程安全、对象重用）。

### 2. 热路径性能持平（预期内）

`acquire()/release()` 的热路径在两种状态下完全相同（都是 vector 的 pop/push 一个指针），因此：

- 单次 acquire 延迟 p50=20ns / p99=31ns 完全一致
- 单线程吞吐量基本一致（75.5M vs 73.3M，噪声范围内）

**重构不是为了让 acquire 更快，而是为了正确的内存管理模型。**

### 3. Placement New 的真实收益（本 benchmark 测不到）

1. **构造阶段**：N 次 malloc → 1 次 `::operator new`，初始化开销从 O(N) 次系统调用降至 1 次。
2. **内存布局**：对象落在连续地址空间，缓存友好（cache-friendly），为缓存优化打基础。
3. **职责分离**：分配（operator new）与构造（placement new）解耦，未来可支持懒构造、按需扩块（Arena）。
4. **生命周期显式化**：`new`/`delete` 的配对变成 `placement new`/`~Task()` + `operator new`/`operator delete`，生命周期完全由 pool 掌控。

### 4. 数据噪声说明

- 线程本地 8 线程 B 比 A 高 30%（404M vs 311M），但单次运行、无重复取样，且竞争/低线程场景未一致提升，**不作为收益证据**。
- 竞争 8 线程 B 略低（31M vs 28.7M），同样在噪声范围内。
- 严格对比需要重复采样（avg/min/max over multiple runs）统一基准。

## 后续行动

- [ ] MemoryPool 支持 expand/reject 策略；benchmark 独立成 `benchmark/memory_pool_benchmark.cpp`
- [ ] 缓存布局审计（连续内存优势需设计可测量的场景）
- [ ] 统一 Benchmark（重复取样、avg/min/max 输出），再回填 README

## 复现方法

```sh
# 状态 B（当前）
cmake --build build -t memory_pool_test
./build/memory_pool_test

# 状态 A（临时还原）
git add -A && git stash push -u -m "backup"
git checkout -- .
cmake --build build -t memory_pool_test
./build/memory_pool_test
git stash pop
```
