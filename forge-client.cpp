#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sys/wait.h>
#include "job-support.h"
#include <syncstream>
#include <arpa/inet.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <thread>
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
        ssize_t sent = send(socket, ptr, bytes, MSG_NOSIGNAL);

        if (sent < 0 && errno == EINTR) continue;
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

        if (received < 0 && errno == EINTR) continue;
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

struct CompileResult {
    bool success;
    bool cached;
    fs::path object;
    CompileResult(bool ok = false, bool hit = false, fs::path path = {})
        : success(ok), cached(hit), object(std::move(path)) {}
};

CompileResult compile_remote(
    const std::string& source_argument,
    const std::vector<std::string>& flags,
    const std::string& host,
    int port,
    const fs::path& object_directory
) {

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

    SocketGuard connection(sock);
    if (!set_socket_timeouts(sock)) return false;

    sockaddr_in worker{};
    worker.sin_family = AF_INET;
    worker.sin_port = htons(port);

    // The command line accepts one worker's IPv4 address.
    inet_pton(
        AF_INET,
        host.c_str(),
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


        return false;
    }

    if (!send_string(
            sock,
            source.generic_string()
        )) {

        return false;
    }

    if (!send_u32(
            sock,
            static_cast<uint32_t>(flags.size())
        )) {

        return false;
    }

    for (const auto& flag : flags) {
        if (!send_string(sock, flag)) {

            return false;
        }
    }

    if (!send_u32(
            sock,
            static_cast<uint32_t>(dependencies.size())
        )) {

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


            return false;
        }

        std::string path = dependency.generic_string();

        if (!send_string(sock, path)) {

            return false;
        }

        if (!send_u32(
                sock,
                static_cast<uint32_t>(file_data.size())
            )) {

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

        return false;
    }

    // 0: worker failure, 1: cached object, 2/3: compiler failure/success.
    if (status == 2 || status == 3) {
        uint32_t exit_code, length;
        if (!recv_u32(sock, exit_code) || !recv_u32(sock, length)
            || length > 1024 * 1024) {
            std::cerr << "Invalid compiler response\n";
            return false;
        }
        std::string diagnostics(length, '\0');
        if (length && !recv_all(sock, diagnostics.data(), length)) return false;
        std::osyncstream(std::cerr) << "[" << source.generic_string() << "] compiler exit status: "
                  << exit_code << '\n' << diagnostics;
        if (status == 2 || exit_code != 0) return false;
    } else if (status != 0 && status != 1) {
        std::cerr << "Unknown worker response\n";
        return false;
    }
    if (status == 0) {
        std::cerr
            << "Remote compilation failed: "
            << source.string()
            << '\n';


        return false;
    }

    std::string object_path_string;

    if (!recv_string(
            sock,
            object_path_string
        )) {

        return false;
    }

    fs::path relative_object =
        fs::path(object_path_string).lexically_normal();

    fs::path expected_object = source;
    expected_object.replace_extension(".o");
    if (!safe_relative_path(relative_object) || relative_object != expected_object) {
        std::cerr << "Worker returned unsafe path\n";

        return false;
    }

    uint32_t object_size;

    if (!recv_u32(sock, object_size)) {

        return false;
    }

    if (object_size > 100 * 1024 * 1024) {
        std::cerr << "Returned object exceeds 100 MiB limit\n";
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

        return false;
    }

    fs::path output_path =
        object_directory / relative_object;

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


        return false;
    }

    output.write(
        object_data.data(),
        object_data.size()
    );

    output.close();
    if (!output) {
        std::error_code error;
        fs::remove(output_path, error);
        throw std::runtime_error("Could not write returned object");
    }

    std::cout
        << "Received "
        << output_path.generic_string()
        << '\n';


    return {true, status == 1, output_path};
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr
            << "Usage:\n"
            << "  ./forge-client [--host IPv4] [--port PORT] [-o executable] [--compile-only] [-j jobs] file1.cpp file2.cpp\n\n"
            << "or:\n"
            << "  ./forge-client [--host IPv4] [--port PORT] [-o executable] [--compile-only] [-j jobs] <compiler flags> -- <source files>\n";

        return 1;
    }

    std::vector<std::string> flags;
    std::vector<std::string> sources;

    std::string host = "127.0.0.1";
    int port = 9000;
    fs::path executable = "app";
    bool compile_only = false;
    std::vector<std::string> link_flags;
    int parallel_jobs = 4;
    int first = 1;
    while (first < argc) {
        std::string option = argv[first];
        if (option.size() > 2 && option.substr(0, 2) == "-j") {
            if (!parse_port(option.substr(2), parallel_jobs) || parallel_jobs > 256) {
                std::cerr << "Job limit must be an integer from 1 to 256\n";
                return 1;
            }
            ++first;
            continue;
        }
        if (option == "--compile-only") { compile_only = true; ++first; continue; }
        if (option != "--host" && option != "--port" && option != "-o"
            && option != "--link-flag" && option != "-j") break;
        if (++first == argc) {
            std::cerr << "Missing value for " << option << '\n';
            return 1;
        }
        if (option == "-j") {
            if (!parse_port(argv[first], parallel_jobs) || parallel_jobs > 256) {
                std::cerr << "Job limit must be an integer from 1 to 256\n";
                return 1;
            }
        }
        else if (option == "--host") host = argv[first];
        else if (option == "-o") executable = argv[first];
        else if (option == "--link-flag") link_flags.push_back(argv[first]);
        else if (!parse_port(argv[first], port)) {
            std::cerr << "Port must be an integer from 1 to 65535\n";
            return 1;
        }
        ++first;
    }
    in_addr address{};
    if (inet_pton(AF_INET, host.c_str(), &address) != 1) {
        std::cerr << "Worker host must be a valid IPv4 address\n";
        return 1;
    }
    int separator = -1;

    for (int i = first; i < argc; ++i) {
        if (std::string(argv[i]) == "--") {
            separator = i;
            break;
        }
    }

    if (separator == -1) {
        flags.push_back("-std=c++20");

        for (int i = first; i < argc; ++i) {
            sources.push_back(argv[i]);
        }
    } else {
        for (int i = first; i < separator; ++i) {
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

    // Reject duplicate object destinations before launching concurrent writes.
    std::set<fs::path> destinations;
    for (const auto& source : sources) {
        fs::path object = fs::path(source).lexically_normal();
        if (!compile_only && fs::absolute(executable).lexically_normal()
            == fs::absolute(object).lexically_normal()) {
            std::cerr << "Executable must not overwrite an input source\n";
            return 1;
        }
        object.replace_extension(".o");
        if (!destinations.insert(object).second) {
            std::cerr << "Duplicate object destination: " << object << '\n';
            return 1;
        }
    }
    const auto started = std::chrono::steady_clock::now();
    JobWorkspace invocation("returned");
    fs::path object_directory = compile_only ? fs::path("returned") : invocation.path;
    const size_t worker_count = std::min(static_cast<size_t>(parallel_jobs), sources.size());
    std::cout << "Parallel job limit: " << worker_count << '\n';
    std::vector<CompileResult> results(sources.size());
    std::atomic<size_t> next_source{0};
    auto run_jobs = [&]() {
        while (true) {
            size_t index = next_source.fetch_add(1);
            if (index >= sources.size()) return;
            try {
                results[index] = compile_remote(sources[index], flags, host, port, object_directory);
            } catch (const std::exception& error) {
                std::osyncstream(std::cerr) << "Job failed: " << sources[index]
                                          << ": " << error.what() << '\n';
            } catch (...) {
                std::osyncstream(std::cerr) << "Job failed: " << sources[index]
                                          << ": unknown error\n";
            }
        }
    };
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    try {
        for (size_t i = 0; i < worker_count; ++i) workers.emplace_back(run_jobs);
    } catch (const std::system_error& error) {
        // Use the calling thread for the missing slot if thread creation fails.
        std::cerr << "Could not start all job threads: " << error.what() << '\n';
        run_jobs();
    }
    for (auto& worker : workers) worker.join();

    size_t compiled = 0, cached = 0, failures = 0;
    std::vector<fs::path> objects;
    for (const auto& result : results) {
        if (!result.success) { ++failures; continue; }
        if (result.cached) ++cached;
        else ++compiled;
        objects.push_back(result.object);
    }
    bool link_failed = false;
    std::string link_status = compile_only ? "not requested" : "skipped";
    if (!compile_only && failures == 0) {
        try {
            fs::path parent = fs::absolute(executable).parent_path();
            JobWorkspace link_workspace(parent);
            fs::path temporary = link_workspace.path / "executable";
            std::string command = "g++ ";
            for (const auto& flag : flags) command += shell_quote(flag) + " ";
            for (const auto& object : objects) command += shell_quote(object.string()) + " ";
            for (const auto& flag : link_flags) command += shell_quote(flag) + " ";
            command += "-o " + shell_quote(temporary.string());
            int status = std::system(command.c_str());
            if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                throw std::runtime_error("Local linking failed; see linker output above");
            }
            fs::rename(temporary, executable);
            link_status = "succeeded";
            std::cout << "Created executable: " << executable.string() << '\n';
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            link_failed = true;
            link_status = "failed";
        }
    }
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    std::cout << "\nBuild summary: compiled=" << compiled << ", cache hits=" << cached
              << ", failures=" << failures << ", link=" << link_status
              << ", elapsed=" << std::fixed << std::setprecision(3) << elapsed << "s\n";
    return failures || link_failed ? 1 : 0;
}