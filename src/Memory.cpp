#include "../include/Memory.h"

Memory::Memory(size_t capacity)
{
    for (size_t i = 0; i < capacity; i++) {
        tasks.push(new Task());
    }
}

Task* Memory::acquire(std::function<void()> func)
{
    if(tasks.empty()) {
        return nullptr;
    } else {
        Task* task = tasks.top();
        tasks.pop();
        task->setFunction(func);
        return task;
    }
}

void Memory::release(Task* task)
{
    task->reset();
    tasks.push(task);
}