#pragma once

#include "RingBuffer.h"

#include<mutex>

template <typename T>
class LockedRingBuffer {
public:
    explicit LockedRingBuffer(size_t capacity) : buffer_(capacity) {}

    bool push(T&& item)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return buffer_.push(std::move(item));
    }

    bool pop(T& item)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return buffer_.pop(item);
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return buffer_.empty();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return buffer_.size();
    }

private:
    mutable std::mutex mtx_;
    RingBuffer<T> buffer_;
};