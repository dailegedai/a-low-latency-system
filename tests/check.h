#pragma once

#include <cstdlib>
#include <iostream>

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL: " #cond " (line " << __LINE__ << ")\n";        \
            std::exit(EXIT_FAILURE);                                           \
        }                                                                      \
    } while (0)
