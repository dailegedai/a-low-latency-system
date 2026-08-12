// #include "../include/Task.h"
// #include "../include/RingBuffer.h"

// #include <functional>
// #include <iostream>
// #include <new>
// #include <optional>
// #include <queue>

// int main() {
//     std::cout << "hardware destructive interference size : "
//               << std::hardware_destructive_interference_size << "\n";   // 预期 64
//     std::cout << "sizeof(Task)                         : " << sizeof(Task) << "\n";
//     std::cout << "alignof(Task)                        : " << alignof(Task) << "\n";
//     std::cout << "offsetof(Task, task_id_)             : "
//               << offsetof(Task, task_id_) << "\n";
//     std::cout << "sizeof(std::function<void()>)        : "
//               << sizeof(std::function<void()>) << "\n";
//     std::cout << "sizeof(std::optional<Task>)          : "
//               << sizeof(std::optional<Task>) << "\n";
//     std::cout << "sizeof(RingBuffer<Task>)             : "
//               << sizeof(RingBuffer<Task>) << "\n";
//     std::cout << "sizeof(std::queue<Task>)             : "
//               << sizeof(std::queue<Task>) << "\n";
//     return 0;
// }