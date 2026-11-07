#include "../include/Task.h"
#include "check.h"

#include <iostream>

int main() {
    int calls = 0;
    Task t([&calls](){
        ++calls;
    });

    t.execute();
    CHECK(calls == 1);
    t.execute();
    CHECK(calls == 2);

    t.reset();
    t.execute();
    CHECK(calls == 2);

    std::cout << "Task execute/reset PASS\n";
    return 0;
}
