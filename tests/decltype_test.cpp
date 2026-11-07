#include <iostream>
#include <type_traits>

#include "check.h"

int add (int a, int b) {
    return a + b;
}

template<typename F>
auto execute(F f) -> decltype(f()) {
    return f();
}

int main() {
    decltype(add(1, 2)) result = 100;
    CHECK(result == 100);

    auto result2 = execute([]() {
        return 123;
    });
    CHECK(result2 == 123);

    using result_type = 
            std::invoke_result_t<
                decltype(add),
                int,
                int
            >;
    result_type x = 999;
    CHECK(x == 999);

    std::cout << "decltype/invoke_result PASS\n";
    return 0;
}
