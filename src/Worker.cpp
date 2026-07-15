#include "../include/Worker.h"

Worker::Worker(std::thread t) : thread_(std::move(t))
{

}

void Worker::join()
{
    if (thread_.joinable()) {
        thread_.join();
    }
}