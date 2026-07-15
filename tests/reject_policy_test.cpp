#include "../include/ThreadPool.h"

#include <iostream>
#include <chrono>

int main()
{
    std::cout << "========== Reject Policy Test (THROW) ==========\n";
    ThreadPool pool(2, 5, RejectPolicy::THROW);

    try
    {
        for (int i = 0; i < 100; i++) 
        {
            pool.submit([i]
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::cout << "task " << i << std::endl;
            });
            std::cout << "submit " << i << std::endl;
        }
    }
    catch(const std::exception& e)
    {
        std::cout << "exception: " << e.what() << std::endl;
    }

    std::cout << "========== Reject Policy Test DONE ==========\n";
}