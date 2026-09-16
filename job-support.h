#pragma once

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

// Each job owns its socket, including when an exception interrupts it.
struct SocketGuard {
    int fd;
    explicit SocketGuard(int socket) : fd(socket) {}
    ~SocketGuard() { if (fd >= 0) close(fd); }
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
};

inline bool set_socket_timeouts(int fd) {
    // Allow time for compilation while still bounding a stalled connection.
    timeval timeout{120, 0};
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0
        && setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
}

struct JobWorkspace {
    std::filesystem::path path;
    JobWorkspace() {
        std::string pattern = "worker-jobs/job-XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        char* directory = mkdtemp(name.data());
        if (!directory) throw std::runtime_error("Could not create job workspace");
        path = directory;
    }
    ~JobWorkspace() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    JobWorkspace(const JobWorkspace&) = delete;
    JobWorkspace& operator=(const JobWorkspace&) = delete;
};
