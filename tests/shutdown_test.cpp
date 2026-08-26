#include "../include/ThreadPool.h"

#include "check.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

int main()
{
    std::cout
        << "========== Shutdown Test ==========\n";

    /*
    ====================================
    Test 1
    Normal shutdown
    ====================================
    */

    {
        std::cout
            << "[TEST1] normal shutdown\n";

        ThreadPool pool(4, 10);

        pool.shutdown();

        CHECK(pool.isStopping());

        std::cout
            << "PASS\n";
    }

    /*
    ====================================
    Test 2
    shutdown idempotent
    ====================================
    */

    {
        std::cout
            << "[TEST2] shutdown twice\n";

        ThreadPool pool(4, 10);

        pool.shutdown();

        pool.shutdown();

        std::cout
            << "PASS\n";
    }

    /*
    ====================================
    Test 3
    all queued tasks executed
    ====================================
    */

    {
        std::cout
            << "[TEST3] graceful shutdown\n";

        ThreadPool pool(4, 10);

        std::atomic<int> counter{0};

        for(int i=0;i<100;i++)
        {
            pool.submit([&counter]()
            {
                counter.fetch_add(1);
            });
        }

        pool.shutdown();

        CHECK(counter.load()==100);

        std::cout
            << "PASS\n";
    }

    /*
    ====================================
    Test 4
    reject submit after shutdown
    ====================================
    */

    {
        std::cout
            << "[TEST4] reject submit\n";

        ThreadPool pool(4, 10);

        pool.shutdown();

        bool exceptionCaught=false;

        try
        {
            pool.submit([](){});
        }
        catch(const std::exception&)
        {
            exceptionCaught=true;
        }

        CHECK(exceptionCaught);

        std::cout
            << "PASS\n";
    }

    /*
    ====================================
    Test 5
    future still works
    ====================================
    */

    {
        std::cout
            << "[TEST5] future\n";

        ThreadPool pool(4, 10);

        auto future=
            pool.submit([]()
            {
                return 123;
            });

        CHECK(
            future.get()==123
        );

        pool.shutdown();

        std::cout
            << "PASS\n";
    }

    /*
    ====================================
    Test 6
    exception propagation
    ====================================
    */

    {
        std::cout
            << "[TEST6] exception propagation\n";

        ThreadPool pool(4, 10);

        auto future=
            pool.submit([]()->int
            {
                throw std::runtime_error(
                    "task exception"
                );
            });

        bool exceptionCaught=false;

        try
        {
            future.get();
        }
        catch(const std::runtime_error&)
        {
            exceptionCaught=true;
        }

        CHECK(exceptionCaught);

        pool.shutdown();

        std::cout
            << "PASS\n";
    }

    /*
    ====================================
    Test 7
    metrics
    ====================================
    */

    {
        std::cout
            << "[TEST7] metrics\n";

        ThreadPool pool(4, 10);

        for(int i=0;i<100;i++)
        {
            pool.submit([](){});
        }

        pool.shutdown();

        CHECK(
            pool.getSubmittedTaskCount()==100
        );

        CHECK(
            pool.getCompletedTaskCount()==100
        );

        std::cout
            << "PASS\n";
    }

    /*
    ====================================
    Test 8
    worker count unchanged
    ====================================
    */

    {
        std::cout
            << "[TEST8] worker count\n";

        ThreadPool pool(8, 20);

        CHECK(
            pool.getThreadCount()==8
        );

        pool.shutdown();

        std::cout
            << "PASS\n";
    }

    /*
    ====================================
    Test 9
    concurrent submit + shutdown (lifetime safety)
    shutdown() 必须等待在途 submit 完成，避免成员销毁时仍有提交访问。
    ====================================
    */

    {
        std::cout
            << "[TEST9] concurrent submit + shutdown\n";

        std::atomic<bool> stop_submit{false};
        std::atomic<int> completed{0};

        {
            ThreadPool pool(1, 2, RejectPolicy::BLOCK);

            std::vector<std::thread> producers;
            for (int t = 0; t < 8; ++t) {
                producers.emplace_back([&] {
                    try {
                        while (!stop_submit.load(std::memory_order_relaxed)) {
                            auto fut = pool.submit([&] {
                                std::this_thread::sleep_for(std::chrono::microseconds(20));
                                completed.fetch_add(1, std::memory_order_relaxed);
                            });
                            (void)fut;
                        }
                    } catch (const std::exception&) {
                        // shutdown 后 submit 抛异常：预期内，生产者退出
                    }
                });
            }

            // 让队列填满、生产者阻塞在 not_full_cv
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            pool.shutdown();   // 必须等待所有在途 submit 完成
            stop_submit.store(true);

            for (auto& th : producers) {
                th.join();
            }
        } // pool 在此析构；此时不应有在途 submit 访问成员

        CHECK(completed.load() > 0);
        std::cout
            << "PASS\n";
    }

    std::cout
        << "\n========== ALL TEST PASSED ==========\n";

    return 0;
}