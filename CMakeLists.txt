cmake_minimum_required(VERSION 3.16)
project(sovd_toolkit CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

option(SOVD_ADAPTER_MOCK "Build the in-memory mock adapter" ON)
option(SOVD_ADAPTER_UDS_DOIP "Build the UDS/DoIP adapter (Phase 2)" OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

# --- core: pure logic, no HTTP/sockets/JSON ---
add_library(sovd_core STATIC
    core/src/adapter.c
    core/src/entity_registry.cpp
    core/src/lock_manager.cpp
)
target_include_directories(sovd_core PUBLIC core/include)
target_link_libraries(sovd_core PUBLIC Threads::Threads)

# --- adapters (statically selected at configure time) ---
set(SOVD_ADAPTER_LIBS "")

if(SOVD_ADAPTER_MOCK)
    add_library(sovd_adapter_mock STATIC adapters/mock/mock_adapter.cpp)
    target_include_directories(sovd_adapter_mock PUBLIC adapters/mock)
    target_link_libraries(sovd_adapter_mock PUBLIC sovd_core)
    list(APPEND SOVD_ADAPTER_LIBS sovd_adapter_mock)
endif()

if(SOVD_ADAPTER_UDS_DOIP)
    message(FATAL_ERROR "SOVD_ADAPTER_UDS_DOIP is a Phase 2 target and not implemented yet")
endif()

# --- server: HTTP layer, no diagnostic logic ---
add_library(sovd_server_lib STATIC
    server/src/routes.cpp
)
target_include_directories(sovd_server_lib PUBLIC server/include third_party)
target_link_libraries(sovd_server_lib PUBLIC sovd_core Threads::Threads)

add_executable(sovd_server server/src/main.cpp)
target_include_directories(sovd_server PRIVATE adapters/mock)
target_link_libraries(sovd_server PRIVATE sovd_server_lib ${SOVD_ADAPTER_LIBS})

# --- tests ---
enable_testing()
add_executable(test_core tests/test_core.cpp)
target_include_directories(test_core PRIVATE tests adapters/mock)
target_link_libraries(test_core PRIVATE sovd_server_lib ${SOVD_ADAPTER_LIBS} Threads::Threads)
add_test(NAME test_core COMMAND test_core)
