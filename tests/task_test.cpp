#include "../include/Task.h"

#include <iostream>

int main() {
    Task t([](){
        std::cout << "hello\n";
    });

    t.execute();
}