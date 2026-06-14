#include "../include/ThreadPool.h"
#include <iostream>
#include <chrono>
#include <exception>

int add (int a, int b) {
    return a + b;
}

int main() {
    ThreadPool pool(4, 100);

    auto future1 = pool.submit([](int a, int b) {
        return a + b;
    }, 1, 2);

    auto future2 = pool.submit([]() {
        return std::string("hello thread pool");
    });

    auto fut = pool.submit(add, 10, 12);

    auto fut1 = pool.submit([]() {
        throw std::runtime_error("error");
        return 1;
    });

    std::cout << fut.get() << "\n";
    std::cout << fut1.get() << std::endl;

    std::cout << future1.get() << std::endl;

    std::cout << future2.get() << std::endl;
}