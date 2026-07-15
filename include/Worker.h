#include <thread>

class Worker {
public:
    explicit Worker(std::thread t);

    void join();
private:
    std::thread thread_;
};