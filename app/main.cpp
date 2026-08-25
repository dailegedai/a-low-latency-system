#include "../include/ThreadPool.h"

#include <iostream>
#include <string>

int add(int a, int b)
{
    return a + b;
}

int main()
{
    ThreadPool pool(4, 100);

    auto future1 = pool.submit([](int a, int b) {
        return a + b;
    }, 1, 2);

    auto future2 = pool.submit([]() {
        return std::string("hello thread pool");
    });

    auto fut = pool.submit(add, 10, 12);

    std::cout << fut.get() << "\n";
    std::cout << future1.get() << "\n";
    std::cout << future2.get() << "\n";

    return 0;
}