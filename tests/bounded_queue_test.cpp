#include "../include/ThreadPool.h"

#include <chrono>
#include <iostream>

int main() {
    ThreadPool pool(2, 5);
    for (int i = 0; i < 20; i++) {
        std::cout << "submit " << i << std::endl;

        pool.submit([i] {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::cout << "submit " << i << "done" << std::endl;
        });
    }
}