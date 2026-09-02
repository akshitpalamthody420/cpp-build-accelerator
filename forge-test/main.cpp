#include <iostream>

#include "math.h"
#include "strings.h"

int main() {
    std::cout << greet("Forge") << '\n';
    std::cout << "10 + 20 = " << add(10, 20) << '\n';
    std::cout << "6 * 7 = " << multiply(6, 7) << '\n';

    return 0;
}  