#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <stdint.h>

class Task {
public:
    explicit Task(std::function<void()> func);

    void execute();

    uint64_t id() const;
    std::chrono::steady_clock::time_point submitTime() const;

private:
    static std::atomic<uint64_t> next_id_;

    uint64_t task_id_;
    std::chrono::steady_clock::time_point submit_time_;
    std::function<void()> func_;
};