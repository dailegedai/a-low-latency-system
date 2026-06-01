#include <iostream>
#include <future>

int main() {
    std::promise<int> p;
    auto fut = p.get_future();

    std::thread t([&p] {
        return p.set_value(123);
    });
    std::cout << fut.get() << std::endl;
    t.join();
}