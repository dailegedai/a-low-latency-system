#pragma once

#include <chrono>
#include <iostream>
#include <string>
#include <vector>


class Benchmark {
public:
    Benchmark(std::string name) : name_(name)
    {
        start_ = std::chrono::steady_clock::now();
    }

    ~Benchmark()
    {
        auto end = std::chrono::steady_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start_);

        std::cout << name_ << ": " << duration.count() << "ms" << std::endl;
    }

private:
    std::string name_;
    std::chrono::steady_clock::time_point start_;
};