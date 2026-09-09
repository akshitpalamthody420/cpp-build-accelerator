#include <arpa/inet.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <netinet/in.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

bool send_all(int socket, const void* buffer, size_t bytes) {
    const char* ptr = static_cast<const char*>(buffer);

    while (bytes > 0) {
        ssize_t sent = send(socket, ptr, bytes, 0);

        if (sent <= 0) {
            return false;
        }

        ptr += sent;
        bytes -= sent;
    }

    return true;
}

bool recv_all(int socket, void* buffer, size_t bytes) {
    char* ptr = static_cast<char*>(buffer);

    while (bytes > 0) {
        ssize_t received = recv(socket, ptr, bytes, 0);

        if (received <= 0) {
            return false;
        }

        ptr += received;
        bytes -= received;
    }

    return true;
}

bool send_u32(int socket, uint32_t value) {
    uint32_t network_value = htonl(value);

    return send_all(
        socket,
        &network_value,
        sizeof(network_value)
    );
}

bool recv_u32(int socket, uint32_t& value) {
    uint32_t network_value;

    if (!recv_all(
            socket,
            &network_value,
            sizeof(network_value)
        )) {
        return false;
    }

    value = ntohl(network_value);
    return true;
}

bool send_string(int socket, const std::string& value) {
    if (!send_u32(
            socket,
            static_cast<uint32_t>(value.size())
        )) {
        return false;
    }

    if (value.empty()) {
        return true;
    }

    return send_all(
        socket,
        value.data(),
        value.size()
    );
}

bool recv_string(int socket, std::string& value) {
    uint32_t length;

    if (!recv_u32(socket, length)) {
        return false;
    }

    if (length > 4096) {
        return false;
    }

    value.resize(length);

    if (length == 0) {
        return true;
    }

    return recv_all(
        socket,
        value.data(),
        length
    );
}

bool safe_relative_path(const fs::path& path) {
    if (path.empty() || path.is_absolute()) {
        return false;
    }

    for (const auto& component : path) {
        if (component == "..") {
            return false;
        }
    }

    return true;
}

bool read_file(
    const fs::path& path,
    std::vector<char>& data
) {
    std::ifstream input(
        path,
        std::ios::binary
    );

    if (!input) {
        return false;
    }

    data.assign(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );

    return true;
}

std::string shell_quote(const std::string& value) {
    std::string result = "'";

    for (char c : value) {
        if (c == '\'') {
            result += "'\\''";
        } else {
            result += c;
        }
    }

    result += "'";
    return result;
}

bool discover_dependencies(
    const fs::path& source,
    const std::vector<std::string>& flags,
    std::vector<fs::path>& dependencies
) {
    std::string command = "g++ ";

    for (const auto& flag : flags) {
        command += shell_quote(flag) + " ";
    }

    command += "-MM " + shell_quote(source.generic_string());

    FILE* pipe = popen(command.c_str(), "r");

    if (!pipe) {
        std::cerr << "Could not run GCC dependency discovery\n";
        return false;
    }

    std::string output;
    std::array<char, 4096> buffer{};

    while (fgets(buffer.data(), buffer.size(), pipe)) {
        output += buffer.data();
    }

    int result = pclose(pipe);

    if (result != 0) {
        std::cerr
            << "Dependency discovery failed for "
            << source.string()
            << '\n';

        return false;
    }

    const std::string continuation = "\\\n";
    size_t position;

    while (
        (position = output.find(continuation))
        != std::string::npos
    ) {
        output.replace(
            position,
            continuation.size(),
            " "
        );
    }

    size_t colon = output.find(':');

    if (colon == std::string::npos) {
        std::cerr << "Could not parse dependency output\n";
        return false;
    }

    std::string dependency_text = output.substr(colon + 1);
    std::istringstream stream(dependency_text);
    std::set<std::string> seen;
    std::string token;

    while (stream >> token) {
        fs::path dependency =
            fs::path(token).lexically_normal();

        if (!safe_relative_path(dependency)) {
            std::cerr
                << "Dependency outside project not supported yet: "
                << dependency.string()
                << '\n';

            return false;
        }

        if (!fs::exists(dependency)) {
            std::cerr
                << "Dependency does not exist: "
                << dependency.string()
                << '\n';

            return false;
        }

        std::string key = dependency.generic_string();

        if (seen.insert(key).second) {
            dependencies.push_back(dependency);
        }
    }

    return !dependencies.empty();
}

bool compile_remote(
    const std::string& source_argument,
    const std::vector<std::string>& flags
) {
    const int PORT = 9000;

    fs::path source =
        fs::path(source_argument).lexically_normal();

    if (!safe_relative_path(source)) {
        std::cerr
            << "Source must be a relative path: "
            << source_argument
            << '\n';

        return false;
    }

    std::vector<fs::path> dependencies;

    if (!discover_dependencies(
            source,
            flags,
            dependencies
        )) {
        return false;
    }

    std::cout
        << "\nJob: "
        << source.generic_string()
        << '\n';

    std::cout << "Flags:";

    for (const auto& flag : flags) {
        std::cout << " " << flag;
    }

    std::cout << '\n';
    std::cout << "Dependencies:\n";

    for (const auto& dependency : dependencies) {
        std::cout
            << "  "
            << dependency.generic_string()
            << '\n';
    }

    int sock =
        socket(
            AF_INET,
            SOCK_STREAM,
            0
        );

    if (sock < 0) {
        std::cerr << "Failed creating socket\n";
        return false;
    }

    sockaddr_in worker{};
    worker.sin_family = AF_INET;
    worker.sin_port = htons(PORT);

    // Current single-worker setup:
    // WSL mirrored networking -> Windows localhost -> VMware NAT forwarding.
    inet_pton(
        AF_INET,
        "127.0.0.1",
        &worker.sin_addr
    );

    if (connect(
            sock,
            reinterpret_cast<sockaddr*>(&worker),
            sizeof(worker)
        ) < 0) {
        std::cerr
            << "Could not connect for "
            << source.string()
            << '\n';

        close(sock);
        return false;
    }

    if (!send_string(
            sock,
            source.generic_string()
        )) {
        close(sock);
        return false;
    }

    if (!send_u32(
            sock,
            static_cast<uint32_t>(flags.size())
        )) {
        close(sock);
        return false;
    }

    for (const auto& flag : flags) {
        if (!send_string(sock, flag)) {
            close(sock);
            return false;
        }
    }

    if (!send_u32(
            sock,
            static_cast<uint32_t>(dependencies.size())
        )) {
        close(sock);
        return false;
    }

    for (const auto& dependency : dependencies) {
        std::vector<char> file_data;

        if (!read_file(
                dependency,
                file_data
            )) {
            std::cerr
                << "Could not read "
                << dependency.string()
                << '\n';

            close(sock);
            return false;
        }

        std::string path = dependency.generic_string();

        if (!send_string(sock, path)) {
            close(sock);
            return false;
        }

        if (!send_u32(
                sock,
                static_cast<uint32_t>(file_data.size())
            )) {
            close(sock);
            return false;
        }

        if (
            !file_data.empty() &&
            !send_all(
                sock,
                file_data.data(),
                file_data.size()
            )
        ) {
            close(sock);
            return false;
        }
    }

    std::cout
        << "Sent "
        << dependencies.size()
        << " project files\n";

    uint32_t status;

    if (!recv_u32(sock, status)) {
        std::cerr << "Failed receiving compile status\n";
        close(sock);
        return false;
    }

    if (status == 0) {
        std::cerr
            << "Remote compilation failed: "
            << source.string()
            << '\n';

        close(sock);
        return false;
    }

    std::string object_path_string;

    if (!recv_string(
            sock,
            object_path_string
        )) {
        close(sock);
        return false;
    }

    fs::path relative_object =
        fs::path(object_path_string).lexically_normal();

    if (!safe_relative_path(relative_object)) {
        std::cerr << "Worker returned unsafe path\n";
        close(sock);
        return false;
    }

    uint32_t object_size;

    if (!recv_u32(sock, object_size)) {
        close(sock);
        return false;
    }

    std::vector<char> object_data(object_size);

    if (
        object_size > 0 &&
        !recv_all(
            sock,
            object_data.data(),
            object_size
        )
    ) {
        close(sock);
        return false;
    }

    fs::path output_path =
        fs::path("returned") / relative_object;

    fs::create_directories(
        output_path.parent_path()
    );

    std::ofstream output(
        output_path,
        std::ios::binary
    );

    if (!output) {
        std::cerr
            << "Could not write "
            << output_path.string()
            << '\n';

        close(sock);
        return false;
    }

    output.write(
        object_data.data(),
        object_data.size()
    );

    std::cout
        << "Received "
        << output_path.generic_string()
        << '\n';

    close(sock);
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr
            << "Usage:\n"
            << "  ./forge-client file1.cpp file2.cpp\n\n"
            << "or:\n"
            << "  ./forge-client <compiler flags> -- <source files>\n";

        return 1;
    }

    std::vector<std::string> flags;
    std::vector<std::string> sources;

    int separator = -1;

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--") {
            separator = i;
            break;
        }
    }

    if (separator == -1) {
        flags.push_back("-std=c++20");

        for (int i = 1; i < argc; ++i) {
            sources.push_back(argv[i]);
        }
    } else {
        for (int i = 1; i < separator; ++i) {
            flags.push_back(argv[i]);
        }

        if (flags.empty()) {
            flags.push_back("-std=c++20");
        }

        for (int i = separator + 1; i < argc; ++i) {
            sources.push_back(argv[i]);
        }
    }

    if (sources.empty()) {
        std::cerr << "No source files provided\n";
        return 1;
    }

    for (const auto& flag : flags) {
        if (
            flag == "-c" ||
            flag == "-o"
        ) {
            std::cerr
                << "Do not pass -c or -o. "
                << "Forge manages those options.\n";

            return 1;
        }
    }

    std::vector<std::future<bool>> jobs;

    for (const auto& source : sources) {
        jobs.push_back(
            std::async(
                std::launch::async,
                compile_remote,
                source,
                flags
            )
        );
    }

    bool success = true;

    for (auto& job : jobs) {
        if (!job.get()) {
            success = false;
        }
    }

    if (!success) {
        std::cerr << "\nRemote build failed.\n";
        return 1;
    }

    std::cout
        << "\nAll remote compilations successful.\n";

    return 0;
}
