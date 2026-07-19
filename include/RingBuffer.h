#pragma once

#include <vector>
#include <optional>
#include <cstddef>
#include <cstdint>

template<typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : buffer_(roundUp(capacity)), head(0), tail(0), capacity_(roundUp(capacity)), size_(0)
    {
    }

    static size_t roundUp(size_t n)
    {
        if (n == 0) return 1;
        --n;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        return n + 1;
    }

    bool push(T&& item)
    {
        if (full()) {
            return false;
        }

        buffer_[tail] = std::move(item);
        // tail = (tail + 1) % capacity_;
        tail = (tail + 1) & (capacity_ - 1);

        ++size_;

        return true;
    }
    bool pop(T& item)
    {
        if (empty()) {
            return false;
        }

        item = std::move(buffer_[head].value());

        buffer_[head].reset();

        // head = (head + 1) % capacity_;
        head = (head + 1) & (capacity_ - 1);

        --size_;

        return true;
    }


    bool empty() const
    {
        return size_ == 0;
    }


    bool full() const
    {
        return size_ == capacity_;
    }


    size_t size() const
    {
        return size_;
    }

private:
    std::vector<std::optional<T>> buffer_;
    size_t head;
    size_t tail;
    size_t capacity_;
    size_t size_;
    
};