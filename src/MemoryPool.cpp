#include "../include/MemoryPool.h"

MemoryPool::MemoryPool(size_t capacity) : capacity_(capacity), storage_(::operator new(capacity * sizeof(Task)))
{
    free_.reserve(capacity);
    auto* p = static_cast<char*>(storage_);
    for (size_t i = 0; i < capacity; ++i) {
        free_.push_back(new (p + i * sizeof(Task)) Task());
    }
}

MemoryPool::~MemoryPool()
{
    for (Task *t : free_) t->~Task();
    ::operator delete(storage_);
}


Task *MemoryPool::acquire()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (free_.empty())
    {
        return nullptr;
    }
    Task *t = free_.back();
    free_.pop_back();
    return t;
}

void MemoryPool::release(Task *task)
{
    std::lock_guard<std::mutex> lock(mtx_);
    free_.push_back(task);
}

size_t MemoryPool::available() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return free_.size();
}

size_t MemoryPool::capacity() const
{
    return capacity_;
}