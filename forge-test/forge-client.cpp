#include <arpa/inet.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

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

int main(int argc, char* argv[]) {

    if (argc != 2) {
        std::cerr
            << "Usage: ./forge-client <source.cpp>\n";

        return 1;
    }

    const int PORT = 9000;

    std::string file_path =
        argv[1];

    // -------------------------------------------------
    // Read source file
    // -------------------------------------------------

    std::ifstream input(
        file_path,
        std::ios::binary
    );

    if (!input) {
        std::cerr
            << "Could not open "
            << file_path
            << '\n';

        return 1;
    }

    std::vector<char> file_data(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>()
    );

    std::string filename =
        std::filesystem::path(file_path)
            .filename()
            .string();

    // -------------------------------------------------
    // Connect to worker
    // -------------------------------------------------

    int sock =
        socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        std::cerr << "Failed creating socket\n";
        return 1;
    }

    sockaddr_in worker{};

    worker.sin_family = AF_INET;
    worker.sin_port = htons(PORT);

    inet_pton(
        AF_INET,
        "127.0.0.1",
        &worker.sin_addr
    );

    std::cout << "Connecting to worker...\n";

    if (connect(
            sock,
            reinterpret_cast<sockaddr*>(&worker),
            sizeof(worker)
        ) < 0) {

        std::cerr << "Could not connect\n";

        close(sock);
        return 1;
    }

    std::cout << "Connected\n";

    // -------------------------------------------------
    // Send source filename length
    // -------------------------------------------------

    uint32_t filename_length =
        static_cast<uint32_t>(
            filename.size()
        );

    uint32_t filename_length_network =
        htonl(filename_length);

    if (!send_all(
            sock,
            &filename_length_network,
            sizeof(filename_length_network)
        )) {

        std::cerr << "Failed sending filename length\n";
        return 1;
    }

    // -------------------------------------------------
    // Send filename
    // -------------------------------------------------

    if (!send_all(
            sock,
            filename.data(),
            filename.size()
        )) {

        std::cerr << "Failed sending filename\n";
        return 1;
    }

    // -------------------------------------------------
    // Send source file size
    // -------------------------------------------------

    uint32_t file_size =
        static_cast<uint32_t>(
            file_data.size()
        );

    uint32_t file_size_network =
        htonl(file_size);

    if (!send_all(
            sock,
            &file_size_network,
            sizeof(file_size_network)
        )) {

        std::cerr << "Failed sending source size\n";
        return 1;
    }

    // -------------------------------------------------
    // Send source bytes
    // -------------------------------------------------

    if (file_size > 0 &&
        !send_all(
            sock,
            file_data.data(),
            file_data.size()
        )) {

        std::cerr << "Failed sending source file\n";
        return 1;
    }

    std::cout
        << "Sent "
        << filename
        << " ("
        << file_data.size()
        << " bytes)\n";

    // -------------------------------------------------
    // Receive compilation status
    //
    // 1 = success
    // 0 = failure
    // -------------------------------------------------

    uint32_t status_network;

    if (!recv_all(
            sock,
            &status_network,
            sizeof(status_network)
        )) {

        std::cerr
            << "Failed receiving compilation status\n";

        close(sock);
        return 1;
    }

    uint32_t status =
        ntohl(status_network);

    if (status == 0) {

        std::cerr
            << "Remote compilation failed\n";

        close(sock);
        return 1;
    }

    std::cout
        << "Remote compilation successful\n";

    // -------------------------------------------------
    // Receive object filename length
    // -------------------------------------------------

    uint32_t object_filename_length_network;

    if (!recv_all(
            sock,
            &object_filename_length_network,
            sizeof(object_filename_length_network)
        )) {

        std::cerr
            << "Failed receiving object filename length\n";

        close(sock);
        return 1;
    }

    uint32_t object_filename_length =
        ntohl(object_filename_length_network);

    // -------------------------------------------------
    // Receive object filename
    // -------------------------------------------------

    std::string object_filename(
        object_filename_length,
        '\0'
    );

    if (!recv_all(
            sock,
            object_filename.data(),
            object_filename_length
        )) {

        std::cerr
            << "Failed receiving object filename\n";

        close(sock);
        return 1;
    }

    object_filename =
        std::filesystem::path(object_filename)
            .filename()
            .string();

    // -------------------------------------------------
    // Receive object size
    // -------------------------------------------------

    uint32_t object_size_network;

    if (!recv_all(
            sock,
            &object_size_network,
            sizeof(object_size_network)
        )) {

        std::cerr
            << "Failed receiving object size\n";

        close(sock);
        return 1;
    }

    uint32_t object_size =
        ntohl(object_size_network);

    // -------------------------------------------------
    // Receive actual .o bytes
    // -------------------------------------------------

    std::vector<char> object_data(
        object_size
    );

    if (object_size > 0 &&
        !recv_all(
            sock,
            object_data.data(),
            object_size
        )) {

        std::cerr
            << "Failed receiving object file\n";

        close(sock);
        return 1;
    }

    // -------------------------------------------------
    // Save returned object file
    // -------------------------------------------------

    std::filesystem::create_directories(
        "returned"
    );

    std::filesystem::path output_path =
        std::filesystem::path("returned")
        / object_filename;

    std::ofstream output(
        output_path,
        std::ios::binary
    );

    if (!output) {
        std::cerr
            << "Could not create returned object file\n";

        close(sock);
        return 1;
    }

    output.write(
        object_data.data(),
        object_data.size()
    );

    output.close();

    std::cout
        << "Received object file: "
        << output_path.string()
        << " ("
        << object_data.size()
        << " bytes)\n";

    close(sock);

    return 0;
}