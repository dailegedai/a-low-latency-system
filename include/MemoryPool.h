#pragma once

#include "Task.h"

#include <mutex>
#include <vector>

class MemoryPool
{
public:
    explicit MemoryPool(size_t capacity);
    ~MemoryPool();
    Task *acquire();
    void release(Task *task);

    size_t capacity() const;
    size_t available() const;
    bool empty() const;

private:
    mutable std::mutex mtx_;
    std::vector<Task *> pool_;
};