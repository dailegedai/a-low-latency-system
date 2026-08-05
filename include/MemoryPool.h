#pragma once

#include "Task.h"

#include <cstddef>
#include <mutex>
#include <vector>

class MemoryPool
{
public:
    explicit MemoryPool(size_t capacity);
    ~MemoryPool();
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    Task *acquire();
    void release(Task *task);

    size_t capacity() const;
    size_t available() const;
    bool empty() const;

private:
    size_t capacity_;
    void* storage_;
    mutable std::mutex mtx_;
    std::vector<Task *> free_;
};