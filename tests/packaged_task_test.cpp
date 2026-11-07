#include "check.h"

#include <iostream>
#include <future>

int main() {
    std::packaged_task<int()> task([]() {
        return 42;
    });

    auto fut = task.get_future();
    task();
    const int value = fut.get();
    CHECK(value == 42);

    std::cout << "packaged_task value PASS\n";
    return 0;
}
