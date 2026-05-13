cmake_minimum_required(VERSION 3.20)

project(redis_clone_phase5 VERSION 0.5.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project is intentionally Windows-only.")
endif()

add_executable(redis_clone
    src/main.cpp
    src/PubSubManager.cpp
    src/RespParser.cpp
    src/Server.cpp
    src/Store.cpp
    src/ThreadPool.cpp
    src/WalLogger.cpp
)

target_include_directories(redis_clone PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(redis_clone PRIVATE cxx_std_17)
set_target_properties(redis_clone PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_compile_definitions(redis_clone PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(redis_clone PRIVATE /W4 /permissive-)
else()
    target_compile_options(redis_clone PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(redis_clone PRIVATE Ws2_32)

add_executable(test_parser tests/test_parser.cpp src/RespParser.cpp)
target_include_directories(test_parser PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(test_parser PRIVATE cxx_std_17)
set_target_properties(test_parser PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
