#include "../include/RingBuffer.h"

#include <cassert>
#include <iostream>

int main()
{
    std::cout << "=========== FIFO TEST ===========\n";
    RingBuffer<int> queue(2);

    assert(queue.empty());

    queue.push(1);
    queue.push(2);

    std::cout << "size of queue is: " << queue.size() << "\n";
    assert(queue.full());
    
    int x = 0;
    queue.pop(x);
    assert(x == 1);
    
    int y = 0;
    queue.pop(y);
    assert(y == 2);

    assert(queue.empty());

    std::cout << "FIFO Test Passed. \n";


    std::cout << "=========== Wrap Aroud TEST ===========\n";

    RingBuffer<int> q2(4);
    q2.push(1);
    q2.push(2);
    q2.push(3);
    q2.push(4);

    std::vector<int> v;
    int a = 0;
    
    q2.pop(a);
    v.emplace_back(a);

    q2.pop(a);
    v.emplace_back(a);

    q2.push(5);
    q2.push(6);

    q2.pop(a);
    v.emplace_back(a);

    q2.pop(a);
    v.emplace_back(a);

    q2.pop(a);
    v.emplace_back(a);

    q2.pop(a);
    v.emplace_back(a);

    for (int i = 0; i < v.size(); i++) {
        assert((i + 1) == v[i]);
        std::cout << v[i] << " ";
    }

    std::cout << std::endl;    
    std::cout << "Wrap Around Test Passed. \n";
}