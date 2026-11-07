#include "check.h"

#include <future>
#include <iostream>
#include <thread>

int main() {
    std::promise<int> p;
    auto fut = p.get_future();

    std::thread t([&p] {
        return p.set_value(123);
    });
    const int value = fut.get();
    CHECK(value == 123);
    t.join();

    std::cout << "future value PASS\n";
    return 0;
}
