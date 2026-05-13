#include "Server.h"
#include "Store.h"

#include <cstdlib>
#include <exception>
#include <iostream>

int main()
{
    try {
        Store store;
        Server server(store);
        server.run();
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "Fatal error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
