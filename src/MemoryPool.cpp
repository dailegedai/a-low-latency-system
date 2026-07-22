#include "../include/MemoryPool.h"

MemoryPool::MemoryPool(size_t capacity)
{
    pool_.reserve(capacity);
    for (size_t i = 0; i < capacity; i++)
    {
        pool_.push_back(new Task());
    }
}

MemoryPool::~MemoryPool()
{
    for (Task *t : pool_)
    {
        delete t;
    }
}

Task *MemoryPool::acquire()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (pool_.empty())
    {
        return nullptr;
    }
    Task *t = pool_.back();
    pool_.pop_back();
    return t;
}

void MemoryPool::release(Task *task)
{
    std::lock_guard<std::mutex> lock(mtx_);
    pool_.push_back(task);
}

size_t MemoryPool::available() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return pool_.size();
}
