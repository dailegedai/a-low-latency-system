#include "../include/Task.h"

Task::Task(std::function<void()> func)
    : func_(std::move(func))
{
}

void Task::setFunction(std::function<void()> func)
{
    func_ = func;
}

void Task::execute()
{
    if (func_) {
        func_();
    }
}

void Task::reset()
{
    func_ = nullptr;
}