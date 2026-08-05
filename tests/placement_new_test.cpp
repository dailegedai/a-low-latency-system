#include "../include/MemoryPool.h"

#include <atomic>
#include <iostream>

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::cerr << "FAIL: " #cond " (line " << __LINE__ << ")\n";    \
            return false;                                                  \
        }                                                                  \
    } while (0)


struct Widget
{
    inline static std::atomic<int> constructed{0};
    inline static std::atomic<int> destroyed{0};
    int value;
    explicit Widget(int v) : value(v)
    {
        constructed.fetch_add(1);
    }
    ~Widget()
    {
        destroyed.fetch_add(1);
    }
};

static bool test_preconstruct_count()
{
    constexpr int N = 8;
    void* raw = ::operator new(N * sizeof(Widget));
    auto* p = static_cast<Widget*>(raw);
    for (int i = 0; i < N; ++i) {
        new (p + i) Widget(i);
    }

    CHECK(Widget::constructed.load() == N);

    for (int i = 0; i < N; ++i) {
        (p + i)->~Widget();
    }
    ::operator delete(raw);
    std::cout << " PASS\n";
    return true;
}

static bool test_acquire_usable()
{
    MemoryPool pool(4);
    Task* t = pool.acquire();
    CHECK(t != nullptr);

    int result = 0;
    t->setFunction([&result]() { result = 123; });
    t->execute();
    CHECK(result == 123);

    pool.release(t);
    std::cout << " PASS\n";
    return true;
}

static bool test_destructor_balance()
{
    constexpr int N = 8;
    void* raw = ::operator new(N * sizeof(Widget));
    auto* p = static_cast<Widget*>(raw);
    for (int i = 0; i < N; ++i) {
        new (p + i) Widget(i);
    }

    for (int i = 0; i < N; ++i) {
        (p + i)->~Widget();
    }

    CHECK(Widget::destroyed.load() == Widget::constructed.load());
    ::operator delete(raw);
    std::cout << " PASS (constructed==" << Widget::constructed.load()
              << " destroyed==" << Widget::destroyed.load() << ")\n";
    return true;
}

static bool test_reuse_same_address()
{
    void* raw = ::operator new(sizeof(Widget));

    auto* w1 = new (raw) Widget(1);
    w1->~Widget();

    auto* w2 = new (raw) Widget(2);
    CHECK(static_cast<void*>(w2) == raw);
    CHECK(w2->value == 2);

    w2->~Widget();
    ::operator delete(raw);
    std::cout << " PASS\n";
    return true;
}

static bool test_pool_reuse()
{
    MemoryPool pool(1);
    Task* t1 = pool.acquire();
    pool.release(t1);
    Task* t2 = pool.acquire();
    CHECK(t1 == t2);
    pool.release(t2);
    std::cout << " PASS\n";
    return true;
}

int main()
{
    std::cout << "========== Placement New Test ==========\n";
    bool ok = true;
    ok &= test_preconstruct_count();
    ok &= test_acquire_usable();
    ok &= test_destructor_balance();
    ok &= test_reuse_same_address();
    ok &= test_pool_reuse();
    std::cout << (ok ? "\n========== ALL TESTS PASSED ==========\n"
                     : "\n========== TESTS FAILED ==========\n");
    return ok ? 0 : 1;
}