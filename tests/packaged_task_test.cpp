#include <iostream>
#include <future>

int main() {
    std::packaged_task<int()> task([]() {
        return 42;
    });

    auto fut = task.get_future();
    task();
    std::cout << fut.get() << std::endl;
}