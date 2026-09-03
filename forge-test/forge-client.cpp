#include <arpa/inet.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
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


bool compile_remote(const std::string& file_path) {
    const int PORT = 9000;

    // -----------------------------------------------------
    // Read local source file
    // -----------------------------------------------------

    std::ifstream input(
        file_path,
        std::ios::binary
    );

    if (!input) {
        std::cerr
            << "Could not open "
            << file_path
            << '\n';

        return false;
    }

    std::vector<char> file_data(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>()
    );

    input.close();

    std::string filename =
        std::filesystem::path(file_path)
            .filename()
            .string();


    // -----------------------------------------------------
    // Create TCP connection
    // -----------------------------------------------------

    int sock =
        socket(
            AF_INET,
            SOCK_STREAM,
            0
        );

    if (sock < 0) {
        std::cerr
            << "Failed creating socket for "
            << filename
            << '\n';

        return false;
    }

    sockaddr_in worker{};

    worker.sin_family = AF_INET;
    worker.sin_port = htons(PORT);

    // WSL mirrored networking
    // -> Windows localhost
    // -> VMware NAT forwarding
    // -> Ubuntu VM worker
    inet_pton(
        AF_INET,
        "127.0.0.1",
        &worker.sin_addr
    );


    std::cout
        << "Connecting for "
        << filename
        << "...\n";


    if (connect(
            sock,
            reinterpret_cast<sockaddr*>(&worker),
            sizeof(worker)
        ) < 0) {

        std::cerr
            << "Could not connect for "
            << filename
            << '\n';

        close(sock);
        return false;
    }


    // -----------------------------------------------------
    // Send source filename length
    // -----------------------------------------------------

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

        std::cerr
            << "Failed sending filename length for "
            << filename
            << '\n';

        close(sock);
        return false;
    }


    // -----------------------------------------------------
    // Send source filename
    // -----------------------------------------------------

    if (!send_all(
            sock,
            filename.data(),
            filename.size()
        )) {

        std::cerr
            << "Failed sending filename "
            << filename
            << '\n';

        close(sock);
        return false;
    }


    // -----------------------------------------------------
    // Send source file size
    // -----------------------------------------------------

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

        std::cerr
            << "Failed sending size for "
            << filename
            << '\n';

        close(sock);
        return false;
    }


    // -----------------------------------------------------
    // Send source bytes
    // -----------------------------------------------------

    if (file_size > 0 &&
        !send_all(
            sock,
            file_data.data(),
            file_data.size()
        )) {

        std::cerr
            << "Failed sending source file "
            << filename
            << '\n';

        close(sock);
        return false;
    }


    std::cout
        << "Sent "
        << filename
        << " ("
        << file_size
        << " bytes)\n";


    // -----------------------------------------------------
    // Receive compilation status
    // -----------------------------------------------------

    uint32_t status_network;

    if (!recv_all(
            sock,
            &status_network,
            sizeof(status_network)
        )) {

        std::cerr
            << "Failed receiving status for "
            << filename
            << '\n';

        close(sock);
        return false;
    }

    uint32_t status =
        ntohl(status_network);


    if (status == 0) {
        std::cerr
            << "Remote compilation failed: "
            << filename
            << '\n';

        close(sock);
        return false;
    }


    std::cout
        << "Remote compilation successful: "
        << filename
        << '\n';


    // -----------------------------------------------------
    // Receive returned object filename length
    // -----------------------------------------------------

    uint32_t object_filename_length_network;

    if (!recv_all(
            sock,
            &object_filename_length_network,
            sizeof(object_filename_length_network)
        )) {

        std::cerr
            << "Failed receiving object filename length for "
            << filename
            << '\n';

        close(sock);
        return false;
    }


    uint32_t object_filename_length =
        ntohl(object_filename_length_network);


    // -----------------------------------------------------
    // Receive returned object filename
    // -----------------------------------------------------

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
            << "Failed receiving object filename for "
            << filename
            << '\n';

        close(sock);
        return false;
    }


    object_filename =
        std::filesystem::path(object_filename)
            .filename()
            .string();


    // -----------------------------------------------------
    // Receive returned object size
    // -----------------------------------------------------

    uint32_t object_size_network;

    if (!recv_all(
            sock,
            &object_size_network,
            sizeof(object_size_network)
        )) {

        std::cerr
            << "Failed receiving object size for "
            << filename
            << '\n';

        close(sock);
        return false;
    }


    uint32_t object_size =
        ntohl(object_size_network);


    // -----------------------------------------------------
    // Receive object-file bytes
    // -----------------------------------------------------

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
            << "Failed receiving object file for "
            << filename
            << '\n';

        close(sock);
        return false;
    }


    // -----------------------------------------------------
    // Save returned .o file
    // -----------------------------------------------------

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
            << "Could not save "
            << output_path.string()
            << '\n';

        close(sock);
        return false;
    }


    output.write(
        object_data.data(),
        object_data.size()
    );

    output.close();


    std::cout
        << "Received "
        << output_path.string()
        << " ("
        << object_size
        << " bytes)\n";


    close(sock);

    return true;
}


int main(int argc, char* argv[]) {

    if (argc < 2) {
        std::cerr
            << "Usage: ./forge-client "
            << "<source1.cpp> <source2.cpp> ...\n";

        return 1;
    }


    // -----------------------------------------------------
    // Start all remote compilation jobs concurrently
    // -----------------------------------------------------

    std::vector<std::future<bool>> jobs;


    for (int i = 1; i < argc; ++i) {

        std::string source =
            argv[i];

        jobs.push_back(
            std::async(
                std::launch::async,
                compile_remote,
                source
            )
        );
    }


    // -----------------------------------------------------
    // Wait for every compilation job to finish
    // -----------------------------------------------------

    bool success = true;


    for (auto& job : jobs) {

        if (!job.get()) {
            success = false;
        }
    }


    if (!success) {

        std::cerr
            << "\nRemote build failed.\n";

        return 1;
    }


    std::cout
        << "\nAll remote compilations successful.\n";

    return 0;
}