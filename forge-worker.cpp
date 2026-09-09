#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <netinet/in.h>
#include <queue>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

std::atomic<uint64_t> next_job_id{1};

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

bool send_u32(int socket, uint32_t value) {
    uint32_t network_value = htonl(value);

    return send_all(
        socket,
        &network_value,
        sizeof(network_value)
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

bool capture_command(
    const std::string& command,
    std::string& output
) {
    FILE* pipe = popen(
        command.c_str(),
        "r"
    );

    if (!pipe) {
        return false;
    }

    output.clear();
    std::array<char, 4096> buffer{};

    while (fgets(
        buffer.data(),
        buffer.size(),
        pipe
    )) {
        output += buffer.data();
    }

    int result = pclose(pipe);
    return result == 0;
}

bool get_compiler_identity(
    std::string& identity
) {
    std::string command =
        "g++ --version | head -n 1; "
        "g++ -dumpmachine; "
        "g++ -dumpfullversion -dumpversion";

    return capture_command(
        command,
        identity
    );
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

bool write_field(
    std::ofstream& output,
    const std::string& value
) {
    output
        << value.size()
        << '\n';

    output.write(
        value.data(),
        value.size()
    );

    output.put('\n');
    return static_cast<bool>(output);
}

bool create_cache_manifest(
    const fs::path& manifest_path,
    const fs::path& workspace,
    const fs::path& relative_source,
    const std::vector<std::string>& flags,
    std::vector<fs::path> dependencies,
    const std::string& compiler_identity
) {
    std::ofstream manifest(
        manifest_path,
        std::ios::binary
    );

    if (!manifest) {
        return false;
    }

    if (!write_field(
            manifest,
            "FORGE_CACHE_V1"
        )) {
        return false;
    }

    if (!write_field(
            manifest,
            compiler_identity
        )) {
        return false;
    }

    if (!write_field(
            manifest,
            relative_source.generic_string()
        )) {
        return false;
    }

    manifest
        << flags.size()
        << '\n';

    for (const auto& flag : flags) {
        if (!write_field(
                manifest,
                flag
            )) {
            return false;
        }
    }

    std::sort(
        dependencies.begin(),
        dependencies.end(),
        [](
            const fs::path& a,
            const fs::path& b
        ) {
            return
                a.generic_string()
                <
                b.generic_string();
        }
    );

    manifest
        << dependencies.size()
        << '\n';

    for (const auto& dependency : dependencies) {
        std::string relative_path =
            dependency.generic_string();

        if (!write_field(
                manifest,
                relative_path
            )) {
            return false;
        }

        fs::path actual_path =
            workspace / dependency;

        std::vector<char> data;

        if (!read_file(
                actual_path,
                data
            )) {
            return false;
        }

        manifest
            << data.size()
            << '\n';

        if (!data.empty()) {
            manifest.write(
                data.data(),
                data.size()
            );
        }

        manifest.put('\n');

        if (!manifest) {
            return false;
        }
    }

    return true;
}

bool compute_sha256(
    const fs::path& file,
    std::string& hash
) {
    std::string output;

    std::string command =
        "sha256sum "
        + shell_quote(file.string());

    if (!capture_command(
            command,
            output
        )) {
        return false;
    }

    std::istringstream stream(output);
    stream >> hash;

    return hash.size() == 64;
}

void handle_client(int client_fd) {
    uint64_t job_id =
        next_job_id.fetch_add(1);

    fs::path workspace =
        fs::path("worker-jobs")
        /
        (
            "job-"
            + std::to_string(job_id)
        );

    fs::create_directories(workspace);

    std::cout
        << "\nJob "
        << job_id
        << " connected\n";

    std::string source_string;

    if (!recv_string(
            client_fd,
            source_string
        )) {
        close(client_fd);
        return;
    }

    fs::path relative_source =
        fs::path(source_string).lexically_normal();

    if (!safe_relative_path(relative_source)) {
        std::cerr << "Unsafe source path\n";
        close(client_fd);
        return;
    }

    uint32_t flag_count;

    if (!recv_u32(
            client_fd,
            flag_count
        )) {
        close(client_fd);
        return;
    }

    if (flag_count > 128) {
        std::cerr << "Too many compiler flags\n";
        close(client_fd);
        return;
    }

    std::vector<std::string> flags;

    for (
        uint32_t i = 0;
        i < flag_count;
        ++i
    ) {
        std::string flag;

        if (!recv_string(
                client_fd,
                flag
            )) {
            close(client_fd);
            return;
        }

        flags.push_back(flag);
    }

    uint32_t file_count;

    if (!recv_u32(
            client_fd,
            file_count
        )) {
        close(client_fd);
        return;
    }

    if (
        file_count == 0 ||
        file_count > 10000
    ) {
        std::cerr << "Invalid file count\n";
        close(client_fd);
        return;
    }

    std::cout
        << "Job "
        << job_id
        << ": "
        << relative_source.generic_string()
        << '\n';

    std::cout << "Flags:";

    for (const auto& flag : flags) {
        std::cout
            << " "
            << flag;
    }

    std::cout << '\n';

    std::vector<fs::path> dependencies;

    for (
        uint32_t i = 0;
        i < file_count;
        ++i
    ) {
        std::string path_string;

        if (!recv_string(
                client_fd,
                path_string
            )) {
            close(client_fd);
            return;
        }

        fs::path relative_path =
            fs::path(path_string).lexically_normal();

        if (!safe_relative_path(relative_path)) {
            std::cerr << "Unsafe dependency path\n";
            close(client_fd);
            return;
        }

        uint32_t file_size;

        if (!recv_u32(
                client_fd,
                file_size
            )) {
            close(client_fd);
            return;
        }

        if (
            file_size >
            100 * 1024 * 1024
        ) {
            std::cerr << "Input file too large\n";
            close(client_fd);
            return;
        }

        std::vector<char> data(file_size);

        if (
            file_size > 0 &&
            !recv_all(
                client_fd,
                data.data(),
                file_size
            )
        ) {
            close(client_fd);
            return;
        }

        fs::path destination =
            workspace / relative_path;

        fs::create_directories(
            destination.parent_path()
        );

        std::ofstream output(
            destination,
            std::ios::binary
        );

        if (!output) {
            std::cerr
                << "Could not create "
                << destination.string()
                << '\n';

            close(client_fd);
            return;
        }

        output.write(
            data.data(),
            data.size()
        );

        output.close();

        dependencies.push_back(
            relative_path
        );

        std::cout
            << "  received "
            << relative_path.generic_string()
            << '\n';
    }

    fs::path source_path =
        workspace / relative_source;

    if (!fs::exists(source_path)) {
        std::cerr << "Primary source missing\n";
        send_u32(client_fd, 0);
        close(client_fd);
        return;
    }

    fs::path relative_object =
        relative_source;

    relative_object.replace_extension(
        ".o"
    );

    fs::path object_path =
        workspace / relative_object;

    fs::create_directories(
        object_path.parent_path()
    );

    std::string compiler_identity;

    if (!get_compiler_identity(
            compiler_identity
        )) {
        std::cerr << "Could not identify compiler\n";
        send_u32(client_fd, 0);
        close(client_fd);
        return;
    }

    fs::path manifest_path =
        workspace / "forge-cache-manifest.bin";

    if (!create_cache_manifest(
            manifest_path,
            workspace,
            relative_source,
            flags,
            dependencies,
            compiler_identity
        )) {
        std::cerr << "Could not create cache manifest\n";
        send_u32(client_fd, 0);
        close(client_fd);
        return;
    }

    std::string cache_key;

    if (!compute_sha256(
            manifest_path,
            cache_key
        )) {
        std::cerr << "Could not compute cache key\n";
        send_u32(client_fd, 0);
        close(client_fd);
        return;
    }

    fs::path cache_path =
        fs::path("cache")
        /
        (
            cache_key
            + ".o"
        );

    bool cache_hit =
        fs::exists(cache_path);

    fs::path object_to_send;

    if (cache_hit) {
        std::cout
            << "CACHE HIT  "
            << relative_source.generic_string()
            << '\n';

        std::cout
            << "Cache key: "
            << cache_key
            << '\n';

        object_to_send = cache_path;
    } else {
        std::cout
            << "CACHE MISS "
            << relative_source.generic_string()
            << '\n';

        std::cout
            << "Cache key: "
            << cache_key
            << '\n';

        std::string command =
            "cd "
            + shell_quote(workspace.string())
            + " && g++ ";

        for (const auto& flag : flags) {
            command +=
                shell_quote(flag)
                + " ";
        }

        command +=
            "-c "
            + shell_quote(
                relative_source.generic_string()
            )
            + " -o "
            + shell_quote(
                relative_object.generic_string()
            );

        std::cout
            << "Compiling job "
            << job_id
            << ": "
            << relative_source.generic_string()
            << '\n';

        int compile_result =
            std::system(command.c_str());

        if (compile_result != 0) {
            std::cerr
                << "Compilation failed: "
                << relative_source.generic_string()
                << '\n';

            send_u32(client_fd, 0);
            close(client_fd);
            return;
        }

        fs::create_directories("cache");

        fs::path temp_cache_path =
            fs::path("cache")
            /
            (
                cache_key
                + ".job-"
                + std::to_string(job_id)
                + ".tmp"
            );

        std::error_code copy_error;

        fs::copy_file(
            object_path,
            temp_cache_path,
            fs::copy_options::overwrite_existing,
            copy_error
        );

        if (copy_error) {
            std::cerr
                << "Warning: could not create cache entry: "
                << copy_error.message()
                << '\n';

            object_to_send =
                object_path;
        } else {
            std::error_code rename_error;

            fs::rename(
                temp_cache_path,
                cache_path,
                rename_error
            );

            if (rename_error) {
                if (fs::exists(cache_path)) {
                    std::error_code remove_error;

                    fs::remove(
                        temp_cache_path,
                        remove_error
                    );
                } else {
                    std::cerr
                        << "Warning: could not publish cache entry: "
                        << rename_error.message()
                        << '\n';
                }
            }

            if (fs::exists(cache_path)) {
                object_to_send =
                    cache_path;
            } else {
                object_to_send =
                    object_path;
            }
        }
    }

    std::vector<char> object_data;

    if (!read_file(
            object_to_send,
            object_data
        )) {
        std::cerr << "Could not read object file\n";
        send_u32(client_fd, 0);
        close(client_fd);
        return;
    }

    if (!send_u32(
            client_fd,
            1
        )) {
        close(client_fd);
        return;
    }

    if (!send_string(
            client_fd,
            relative_object.generic_string()
        )) {
        close(client_fd);
        return;
    }

    if (!send_u32(
            client_fd,
            static_cast<uint32_t>(
                object_data.size()
            )
        )) {
        close(client_fd);
        return;
    }

    if (
        !object_data.empty() &&
        !send_all(
            client_fd,
            object_data.data(),
            object_data.size()
        )
    ) {
        close(client_fd);
        return;
    }

    std::cout
        << "Finished job "
        << job_id
        << ": "
        << relative_object.generic_string();

    if (cache_hit) {
        std::cout << " [cached]";
    }

    std::cout << '\n';

    close(client_fd);
}

class ThreadPool {
public:
    ThreadPool(
        size_t worker_count,
        size_t max_queue_size
    )
        : max_queue_size_(max_queue_size)
    {
        for (size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back(
                [this]() {
                    worker_loop();
                }
            );
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }

        condition_.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    bool submit(int client_fd) {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (
                stopping_ ||
                jobs_.size() >= max_queue_size_
            ) {
                return false;
            }

            jobs_.push(client_fd);
        }

        condition_.notify_one();
        return true;
    }

private:
    void worker_loop() {
        while (true) {
            int client_fd;

            {
                std::unique_lock<std::mutex> lock(mutex_);

                condition_.wait(
                    lock,
                    [this]() {
                        return
                            stopping_ ||
                            !jobs_.empty();
                    }
                );

                if (
                    stopping_ &&
                    jobs_.empty()
                ) {
                    return;
                }

                client_fd = jobs_.front();
                jobs_.pop();
            }

            handle_client(client_fd);
        }
    }

    std::vector<std::thread> workers_;
    std::queue<int> jobs_;
    std::mutex mutex_;
    std::condition_variable condition_;
    size_t max_queue_size_;
    bool stopping_ = false;
};

int main() {
    const int PORT = 9000;

    fs::create_directories(
        "worker-jobs"
    );

    fs::create_directories(
        "cache"
    );

    int server_fd =
        socket(
            AF_INET,
            SOCK_STREAM,
            0
        );

    if (server_fd < 0) {
        std::cerr
            << "Failed creating socket\n";

        return 1;
    }

    int reuse = 1;

    setsockopt(
        server_fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse,
        sizeof(reuse)
    );

    sockaddr_in address{};

    address.sin_family =
        AF_INET;

    address.sin_addr.s_addr =
        INADDR_ANY;

    address.sin_port =
        htons(PORT);

    if (
        bind(
            server_fd,
            reinterpret_cast<sockaddr*>(
                &address
            ),
            sizeof(address)
        ) < 0
    ) {
        std::cerr << "Bind failed\n";
        close(server_fd);
        return 1;
    }

    if (
        listen(
            server_fd,
            64
        ) < 0
    ) {
        std::cerr << "Listen failed\n";
        close(server_fd);
        return 1;
    }

    std::cout
        << "Forge worker listening on port "
        << PORT
        << "...\n";

    std::cout
        << "Cache directory: "
        << fs::absolute(
            "cache"
        ).string()
        << '\n';

    size_t worker_count =
        std::thread::hardware_concurrency();

    if (worker_count == 0) {
        worker_count = 4;
    }

    const size_t MAX_QUEUE_SIZE = 64;

    ThreadPool pool(
        worker_count,
        MAX_QUEUE_SIZE
    );

    std::cout
        << "Worker threads: "
        << worker_count
        << '\n';

    std::cout
        << "Maximum queued jobs: "
        << MAX_QUEUE_SIZE
        << '\n';

    while (true) {
        int client_fd =
            accept(
                server_fd,
                nullptr,
                nullptr
            );

        if (client_fd < 0) {
            std::cerr << "Accept failed\n";
            continue;
        }

        if (!pool.submit(client_fd)) {
            std::cerr
                << "Job queue full - rejecting connection\n";

            close(client_fd);
        }
    }

    close(server_fd);
    return 0;
}
