#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: forge <source1.cpp> <source2.cpp> ...\n";
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        std::string source = argv[i];

        if (source.size() < 4 || source.substr(source.size() - 4) != ".cpp") {
            std::cerr << "Skipping invalid source file: " << source << '\n';
            continue;
        }

        std::string output = source.substr(0, source.size() - 4) + ".o";

        std::string command =
            "g++ -std=c++20 -c " + source + " -o " + output;

        std::cout << "Compiling: " << source << '\n';

        int result = std::system(command.c_str());

        if (result != 0) {
            std::cerr << "Compilation failed: " << source << '\n';
            return 1;
        }

        std::cout << "Created: " << output << '\n';
    }

    return 0;
}