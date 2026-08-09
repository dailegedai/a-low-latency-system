#pragma once

#include "Task.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

class MemoryPool
{
public:
    explicit MemoryPool(size_t capacity, bool expandable = false);
    ~MemoryPool();
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    Task *acquire();
    void release(Task *task);

    size_t capacity() const;
    size_t available() const;
    size_t blockCount() const;
    bool empty() const;

private:
    struct Block {
        void* storage;
        size_t capacity;
        explicit Block(size_t cap);
        ~Block();
        Block(const Block&) = delete;
        Block& operator=(const Block&) = delete;
    };

    void addBlock(size_t capacity);

    bool expandable_;
    size_t initial_capacity_;
    mutable std::mutex mtx_;
    std::vector<std::unique_ptr<Block>> blocks_;
    std::vector<Task *> free_;
};