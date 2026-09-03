#include <arpa/inet.h>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>


// ---------------------------------------------------------
// Receive exactly 'bytes' bytes from a socket
// ---------------------------------------------------------
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


// ---------------------------------------------------------
// Send exactly 'bytes' bytes through a socket
// ---------------------------------------------------------
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


// ---------------------------------------------------------
// Handle ONE compilation request
//
// Each connected forge-client gets its own thread running
// this function.
// ---------------------------------------------------------
void handle_client(int client_fd) {

    std::cout << "\nClient connected\n";


    // -----------------------------------------------------
    // Receive source filename length
    // -----------------------------------------------------

    uint32_t filename_length_network;

    if (!recv_all(
            client_fd,
            &filename_length_network,
            sizeof(filename_length_network)
        )) {

        std::cerr
            << "Failed receiving filename length\n";

        close(client_fd);
        return;
    }

    uint32_t filename_length =
        ntohl(filename_length_network);


    // -----------------------------------------------------
    // Receive source filename
    // -----------------------------------------------------

    std::string filename(
        filename_length,
        '\0'
    );

    if (!recv_all(
            client_fd,
            filename.data(),
            filename_length
        )) {

        std::cerr
            << "Failed receiving filename\n";

        close(client_fd);
        return;
    }

    // Strip any directories supplied by the client.
    filename =
        std::filesystem::path(filename)
            .filename()
            .string();


    // -----------------------------------------------------
    // Receive source file size
    // -----------------------------------------------------

    uint32_t file_size_network;

    if (!recv_all(
            client_fd,
            &file_size_network,
            sizeof(file_size_network)
        )) {

        std::cerr
            << "Failed receiving file size\n";

        close(client_fd);
        return;
    }

    uint32_t file_size =
        ntohl(file_size_network);


    // -----------------------------------------------------
    // Receive source file bytes
    // -----------------------------------------------------

    std::vector<char> file_data(file_size);

    if (file_size > 0 &&
        !recv_all(
            client_fd,
            file_data.data(),
            file_size
        )) {

        std::cerr
            << "Failed receiving file contents\n";

        close(client_fd);
        return;
    }


    // -----------------------------------------------------
    // Save source file
    //
    // Example:
    // math.cpp -> received/math.cpp
    // -----------------------------------------------------

    std::filesystem::path source_path =
        std::filesystem::path("received")
        / filename;

    std::ofstream source_output(
        source_path,
        std::ios::binary
    );

    if (!source_output) {

        std::cerr
            << "Failed creating source file: "
            << source_path.string()
            << '\n';

        close(client_fd);
        return;
    }

    source_output.write(
        file_data.data(),
        file_data.size()
    );

    source_output.close();

    std::cout
        << "Received: "
        << source_path.string()
        << " ("
        << file_size
        << " bytes)\n";


    // -----------------------------------------------------
    // Create object-file path
    //
    // received/math.cpp
    //        ↓
    // received/math.o
    // -----------------------------------------------------

    std::filesystem::path object_path =
        source_path;

    object_path.replace_extension(".o");


    // -----------------------------------------------------
    // Build compiler command
    // -----------------------------------------------------

    std::string command =
        "g++ -std=c++20 -c \"" +
        source_path.string() +
        "\" -o \"" +
        object_path.string() +
        "\"";

    std::cout
        << "Compiling: "
        << filename
        << '\n';


    // -----------------------------------------------------
    // Run g++
    // -----------------------------------------------------

    int compile_result =
        std::system(command.c_str());


    // -----------------------------------------------------
    // Send compilation status
    //
    // 1 = compilation successful
    // 0 = compilation failed
    // -----------------------------------------------------

    uint32_t status =
        (compile_result == 0)
            ? 1
            : 0;

    uint32_t status_network =
        htonl(status);

    if (!send_all(
            client_fd,
            &status_network,
            sizeof(status_network)
        )) {

        std::cerr
            << "Failed sending compilation status\n";

        close(client_fd);
        return;
    }


    // -----------------------------------------------------
    // Stop here if g++ failed
    // -----------------------------------------------------

    if (compile_result != 0) {

        std::cerr
            << "Compilation failed: "
            << filename
            << '\n';

        close(client_fd);
        return;
    }

    std::cout
        << "Compilation successful: "
        << filename
        << '\n';


    // -----------------------------------------------------
    // Open generated .o file
    // -----------------------------------------------------

    std::ifstream object_input(
        object_path,
        std::ios::binary
    );

    if (!object_input) {

        std::cerr
            << "Could not open object file: "
            << object_path.string()
            << '\n';

        close(client_fd);
        return;
    }


    // -----------------------------------------------------
    // Read entire .o file into memory
    // -----------------------------------------------------

    std::vector<char> object_data(
        (std::istreambuf_iterator<char>(object_input)),
        std::istreambuf_iterator<char>()
    );

    object_input.close();


    // -----------------------------------------------------
    // Object filename
    //
    // received/math.o -> math.o
    // -----------------------------------------------------

    std::string object_filename =
        object_path.filename().string();


    // -----------------------------------------------------
    // Send object filename length
    // -----------------------------------------------------

    uint32_t object_filename_length =
        static_cast<uint32_t>(
            object_filename.size()
        );

    uint32_t object_filename_length_network =
        htonl(object_filename_length);

    if (!send_all(
            client_fd,
            &object_filename_length_network,
            sizeof(object_filename_length_network)
        )) {

        std::cerr
            << "Failed sending object filename length\n";

        close(client_fd);
        return;
    }


    // -----------------------------------------------------
    // Send object filename
    // -----------------------------------------------------

    if (!send_all(
            client_fd,
            object_filename.data(),
            object_filename.size()
        )) {

        std::cerr
            << "Failed sending object filename\n";

        close(client_fd);
        return;
    }


    // -----------------------------------------------------
    // Send object-file size
    // -----------------------------------------------------

    uint32_t object_size =
        static_cast<uint32_t>(
            object_data.size()
        );

    uint32_t object_size_network =
        htonl(object_size);

    if (!send_all(
            client_fd,
            &object_size_network,
            sizeof(object_size_network)
        )) {

        std::cerr
            << "Failed sending object size\n";

        close(client_fd);
        return;
    }


    // -----------------------------------------------------
    // Send actual .o bytes
    // -----------------------------------------------------

    if (object_size > 0 &&
        !send_all(
            client_fd,
            object_data.data(),
            object_data.size()
        )) {

        std::cerr
            << "Failed sending object file\n";

        close(client_fd);
        return;
    }


    std::cout
        << "Finished: "
        << filename
        << " -> "
        << object_filename
        << " ("
        << object_data.size()
        << " bytes)\n";


    // -----------------------------------------------------
    // Done with this client
    // -----------------------------------------------------

    close(client_fd);
}


// =========================================================
// MAIN SERVER THREAD
// =========================================================

int main() {

    const int PORT = 9000;


    // -----------------------------------------------------
    // Create TCP server socket
    // -----------------------------------------------------

    int server_fd =
        socket(
            AF_INET,
            SOCK_STREAM,
            0
        );

    if (server_fd < 0) {

        std::cerr
            << "Failed to create socket\n";

        return 1;
    }


    // -----------------------------------------------------
    // Allow quick server restart
    // -----------------------------------------------------

    int reuse = 1;

    setsockopt(
        server_fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse,
        sizeof(reuse)
    );


    // -----------------------------------------------------
    // Configure server address
    // -----------------------------------------------------

    sockaddr_in address{};

    address.sin_family =
        AF_INET;

    address.sin_addr.s_addr =
        INADDR_ANY;

    address.sin_port =
        htons(PORT);


    // -----------------------------------------------------
    // Bind to port 9000
    // -----------------------------------------------------

    if (bind(
            server_fd,
            reinterpret_cast<sockaddr*>(
                &address
            ),
            sizeof(address)
        ) < 0) {

        std::cerr
            << "Bind failed\n";

        close(server_fd);
        return 1;
    }


    // -----------------------------------------------------
    // Start listening
    // -----------------------------------------------------

    if (listen(server_fd, 10) < 0) {

        std::cerr
            << "Listen failed\n";

        close(server_fd);
        return 1;
    }


    // -----------------------------------------------------
    // Make folder for incoming source files
    // -----------------------------------------------------

    std::filesystem::create_directories(
        "received"
    );


    std::cout
        << "Forge worker listening on port "
        << PORT
        << "...\n";


    // -----------------------------------------------------
    // Accept clients forever
    // -----------------------------------------------------

    while (true) {

        int client_fd =
            accept(
                server_fd,
                nullptr,
                nullptr
            );

        if (client_fd < 0) {

            std::cerr
                << "Accept failed\n";

            continue;
        }


        // -------------------------------------------------
        // IMPORTANT:
        //
        // Give this client to another thread.
        //
        // main thread immediately returns to accept(),
        // allowing another compilation request to arrive.
        // -------------------------------------------------

        std::thread(
            handle_client,
            client_fd
        ).detach();
    }


    close(server_fd);

    return 0;
}