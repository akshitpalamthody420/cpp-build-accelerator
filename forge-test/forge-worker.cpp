#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    const int PORT = 9000;

    // Create a TCP socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd == -1) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // Attach socket to port 9000
    if (bind(
            server_fd,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ) == -1) {
        std::cerr << "Bind failed\n";
        close(server_fd);
        return 1;
    }

    // Start waiting for connections
    if (listen(server_fd, 5) == -1) {
        std::cerr << "Listen failed\n";
        close(server_fd);
        return 1;
    }

    std::cout << "Forge worker listening on port "
              << PORT << "...\n";

    while (true) {
        sockaddr_in client_address{};
        socklen_t client_size = sizeof(client_address);

        // Wait here until somebody connects
        int client_fd = accept(
            server_fd,
            reinterpret_cast<sockaddr*>(&client_address),
            &client_size
        );

        if (client_fd == -1) {
            std::cerr << "Accept failed\n";
            continue;
        }

        char buffer[1024]{};

        int bytes_received =
            recv(client_fd, buffer, sizeof(buffer) - 1, 0);

        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';

            std::cout << "Received: "
                      << buffer << '\n';

            const char* response = "READY";

            send(
                client_fd,
                response,
                std::strlen(response),
                0
            );
        }

        close(client_fd);
    }
}