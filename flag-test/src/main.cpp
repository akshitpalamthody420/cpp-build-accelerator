#include <iostream>
#include "settings.h"

#ifndef FACTOR
#define FACTOR 1
#endif

int main() {
    std::cout << BASE_VALUE * FACTOR << '\n';
    return 0;
}
