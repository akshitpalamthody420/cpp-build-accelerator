#include <iostream>

int engine_parser();
int utils_parser();
int http_client();
int http_server();
int auth_handler();
int payments_handler();

int main() {
    std::cout << "engine parser: " << engine_parser() << '\n';
    std::cout << "utils parser: " << utils_parser() << '\n';
    std::cout << "http client: " << http_client() << '\n';
    std::cout << "http server: " << http_server() << '\n';
    std::cout << "auth handler: " << auth_handler() << '\n';
    std::cout << "payments handler: " << payments_handler() << '\n';

    int total =
        engine_parser()
        + utils_parser()
        + http_client()
        + http_server()
        + auth_handler()
        + payments_handler();

    std::cout << "total: " << total << '\n';

    return 0;
}
