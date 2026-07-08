#include "../include/Task.h"

std::atomic<uint64_t> Task::next_id_{0};

Task::Task(std::function<void()> func)
    : task_id_(next_id_.fetch_add(1))
    , submit_time_(std::chrono::steady_clock::now())
    , func_(std::move(func))
{
}

void Task::execute() {
    if (func_) {
        func_();
    }
}

uint64_t Task::id() const {
    return task_id_;
}

std::chrono::steady_clock::time_point Task::submitTime() const {
    return submit_time_;
}