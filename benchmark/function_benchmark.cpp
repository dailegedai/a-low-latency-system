#include <chrono>
#include <functional>
#include <iostream>

int main() {
    constexpr int N = 10000000;
    volatile int count = 0;

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < N; i++) {
        std::function<void()> f = [&count](){
            count++;
        };
        f();
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "construct std::function * 10000000 " << duration.count() << " ms\n";
}