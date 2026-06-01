#include <iostream>
#include <type_traits>

int add (int a, int b) {
    return a + b;
}

template<typename F>
auto execute(F f) -> decltype(f()) {
    return f();
}

int main() {
    decltype(add(1, 2)) result = 100;
    std::cout << result << std::endl;

    auto result2 = execute([]() {
        return 123;
    });
    std::cout << result2 << std::endl;

    using result_type = 
            std::invoke_result_t<
                decltype(add),
                int,
                int
            >;
    result_type x = 999;
    std::cout << x << std::endl;
}