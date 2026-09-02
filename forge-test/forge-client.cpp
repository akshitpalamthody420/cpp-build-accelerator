#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    const int PORT = 9000;

    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock == -1) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    sockaddr_in worker{};

    worker.sin_family = AF_INET;
    worker.sin_port = htons(PORT);

    // 127.0.0.1 means "this computer"
    inet_pton(
        AF_INET,
        "127.0.0.1",
        &worker.sin_addr
    );

    if (connect(
            sock,
            reinterpret_cast<sockaddr*>(&worker),
            sizeof(worker)
        ) == -1) {
        std::cerr << "Could not connect to worker\n";
        close(sock);
        return 1;
    }

    const char* message = "PING";

    send(
        sock,
        message,
        std::strlen(message),
        0
    );

    char buffer[1024]{};

    int bytes_received =
        recv(sock, buffer, sizeof(buffer) - 1, 0);

    if (bytes_received > 0) {
        buffer[bytes_received] = '\0';

        std::cout << "Worker replied: "
                  << buffer << '\n';
    }

    close(sock);
}