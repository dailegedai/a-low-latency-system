#include "../include/ThreadPool.h"

#include <iostream>
#include <cassert>
#include <chrono>

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

        assert(pool.isStopping());

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

        assert(counter.load()==100);

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

        assert(exceptionCaught);

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

        assert(
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

        assert(exceptionCaught);

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

        assert(
            pool.getSubmittedTaskCount()==100
        );

        assert(
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

        assert(
            pool.getThreadCount()==8
        );

        pool.shutdown();

        std::cout
            << "PASS\n";
    }

    std::cout
        << "\n========== ALL TEST PASSED ==========\n";

    return 0;
}