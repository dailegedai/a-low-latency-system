#include <chrono>
#include <iostream>

int main() {
    constexpr int N = 10000000;
    volatile int count = 0;

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < N; i++) {
        auto f = [&count](){
            count++;
        };
        f();
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "construct lambda * 10000000 " << duration.count() << " ms\n";
}