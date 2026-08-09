#include "../include/MemoryPool.h"

MemoryPool::MemoryPool(size_t capacity, bool expandable) : initial_capacity_(capacity), expandable_(expandable)
{
    free_.reserve(capacity);
    addBlock(capacity);
}

MemoryPool::Block::Block(size_t cap) : storage(::operator new(cap * sizeof(Task))), capacity(cap)
{

}

MemoryPool::Block::~Block()
{
    ::operator delete(storage);
}

MemoryPool::~MemoryPool()
{
    for (auto& blk: blocks_) {
        auto* p = static_cast<Task*>(blk->storage);
        for (size_t i = 0; i < blk->capacity; ++i) {
            (p + i)->~Task();
        }
    }
}

void MemoryPool::addBlock(size_t capacity)
{
    blocks_.push_back(std::make_unique<Block>(capacity));
    auto* p = static_cast<char*>(blocks_.back()->storage);
    for (size_t i = 0; i < capacity; ++i) {
        free_.push_back(new (p + i * sizeof(Task)) Task());
    }
}

Task *MemoryPool::acquire()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (free_.empty())
    {
        if (!expandable_) {
            return nullptr;
        }
        size_t grow = initial_capacity_ > 0 ? initial_capacity_ : 16;
        addBlock(grow);
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
    std::lock_guard<std::mutex> lock(mtx_);
    size_t total = 0;
    for (auto& blk : blocks_) {
        total += blk->capacity;
    }
    return total;
}

size_t MemoryPool::blockCount() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return blocks_.size();
}

bool MemoryPool::empty() const
{
    return available() == 0;
}