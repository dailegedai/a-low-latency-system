#pragma once

#include <functional>

class Task {
public:
    Task() = default;
    explicit Task(std::function<void()> func);

    void setFunction(std::function<void()> func);

    void execute();
    void reset();

private:
    std::function<void()> func_;
};