#include "Task.h"

#include <iostream>
#include <stack>
#include <thread>

class Memory {
public:
    explicit Memory(size_t capacity);
    Task* acquire(std::function<void()> func);
    void release(Task* task);

private:
    std::stack<Task*> tasks;
};