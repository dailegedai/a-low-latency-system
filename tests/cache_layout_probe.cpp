#include "../include/ThreadPool.h"
#include "../include/Task.h"
#include "../include/RingBuffer.h"

#include <cstddef>
#include <functional>
#include <iostream>
#include <new>
#include <optional>
#include <queue>

// 注意：offsetof 无法访问 private 成员，故不对 Task 的字段做 offset 探测。

int main() {
    std::cout << "hardware destructive interference size : "
              << std::hardware_destructive_interference_size << "\n";   // 预期 64
    std::cout << "sizeof(Task)                         : " << sizeof(Task) << "\n";
    std::cout << "alignof(Task)                        : " << alignof(Task) << "\n";
    std::cout << "sizeof(std::function<void()>)        : "
              << sizeof(std::function<void()>) << "\n";
    std::cout << "sizeof(std::optional<Task>)          : "
              << sizeof(std::optional<Task>) << "\n";
    std::cout << "sizeof(RingBuffer<Task>)             : "
              << sizeof(RingBuffer<Task>) << "\n";
    std::cout << "sizeof(std::queue<Task>)             : "
              << sizeof(std::queue<Task>) << "\n";
    std::cout << "sizeof(ThreadPool)                   : "
              << sizeof(ThreadPool) << "\n";

    return 0;
}