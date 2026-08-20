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

# mDNS (Phase 5) needs a real client library, not something worth hand-
# rolling raw DNS-SD packet parsing for — avahi-client is the standard
# Linux one and is what's actually installed here. Auto-detect rather than
# defaulting ON unconditionally: not every dev machine has avahi, and the
# rest of Phase 5 (SDK, CLI, RAII locks) must build without it.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(AVAHI_CLIENT QUIET avahi-client)
endif()
option(SOVD_CLIENT_MDNS "Build mDNS discovery/advertising (needs avahi-client)" ${AVAHI_CLIENT_FOUND})

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

# --- catalog: DID/operation YAML definitions, shared by server and adapters ---
# yaml-cpp is fetched rather than vendored as a single header: the catalog
# schema (CLAUDE.md) uses flow-style YAML ({ bytes: 2, ... }), which rules
# out header-only minimal parsers (mini-yaml, tried first, has no flow-style
# support at all). yaml-cpp is the standard, well-tested choice that handles
# the full spec.
include(FetchContent)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG yaml-cpp-0.9.0
)
FetchContent_MakeAvailable(yaml-cpp)

add_library(sovd_catalog STATIC
    catalog/src/did_catalog.cpp
)
target_include_directories(sovd_catalog PUBLIC catalog/include)
target_link_libraries(sovd_catalog PUBLIC yaml-cpp::yaml-cpp)

# --- adapters (statically selected at configure time) ---
set(SOVD_ADAPTER_LIBS "")

if(SOVD_ADAPTER_MOCK)
    add_library(sovd_adapter_mock STATIC adapters/mock/mock_adapter.cpp)
    target_include_directories(sovd_adapter_mock PUBLIC adapters/mock)
    target_link_libraries(sovd_adapter_mock PUBLIC sovd_core)
    list(APPEND SOVD_ADAPTER_LIBS sovd_adapter_mock)
endif()

if(SOVD_ADAPTER_UDS_DOIP)
    add_library(sovd_adapter_uds_doip STATIC
        adapters/uds_doip/src/doip_protocol.cpp
        adapters/uds_doip/src/doip_transport.cpp
        adapters/uds_doip/src/uds_services.cpp
        adapters/uds_doip/src/nrc_map.cpp
        adapters/uds_doip/src/session_manager.cpp
        adapters/uds_doip/src/uds_doip_adapter.cpp
    )
    target_include_directories(sovd_adapter_uds_doip PUBLIC adapters/uds_doip/include)
    target_include_directories(sovd_adapter_uds_doip PRIVATE third_party)
    target_link_libraries(sovd_adapter_uds_doip PUBLIC sovd_core sovd_catalog Threads::Threads)
    list(APPEND SOVD_ADAPTER_LIBS sovd_adapter_uds_doip)
endif()

# --- server: HTTP layer, no diagnostic logic ---
add_library(sovd_server_lib STATIC
    server/src/routes.cpp
    server/src/mqtt_publisher.cpp
    server/src/stream_hub.cpp
)
target_include_directories(sovd_server_lib PUBLIC server/include third_party)
target_link_libraries(sovd_server_lib PUBLIC sovd_core sovd_catalog Threads::Threads)

# --- config loader (Phase 4): YAML topology -> registry/router. Instantiates
# concrete adapters directly (same responsibility main.cpp's hardcoded
# build_topology() already had), so it's a separate target from
# sovd_server_lib rather than baked into it — sovd_server_lib stays
# adapter-agnostic per CLAUDE.md's layering rule, and this is shared by both
# sovd_server and test_core instead of duplicating the source in each.
add_library(sovd_config_loader STATIC server/src/config_loader.cpp)
target_include_directories(sovd_config_loader PRIVATE adapters/mock)
target_link_libraries(sovd_config_loader PUBLIC sovd_server_lib sovd_catalog ${SOVD_ADAPTER_LIBS})
if(SOVD_ADAPTER_UDS_DOIP)
    target_compile_definitions(sovd_config_loader PRIVATE SOVD_HAVE_UDS_DOIP=1)
endif()

add_executable(sovd_server server/src/main.cpp)
target_include_directories(sovd_server PRIVATE adapters/mock)
target_link_libraries(sovd_server PRIVATE sovd_server_lib sovd_config_loader ${SOVD_ADAPTER_LIBS})

# --- client SDK (Phase 5): typed wrappers, RAII locks, retry/backoff ---
add_library(sovd_client STATIC
    client/src/sovd_client.cpp
    client/src/lock_guard.cpp
)
target_include_directories(sovd_client PUBLIC client/include third_party)
target_link_libraries(sovd_client PUBLIC Threads::Threads)
if(SOVD_CLIENT_MDNS)
    target_sources(sovd_client PRIVATE client/src/mdns_discovery.cpp)
    target_link_libraries(sovd_client PUBLIC ${AVAHI_CLIENT_LIBRARIES})
    target_include_directories(sovd_client PUBLIC ${AVAHI_CLIENT_INCLUDE_DIRS})
    target_compile_definitions(sovd_client PUBLIC SOVD_HAVE_MDNS=1)

    add_library(sovd_mdns_advertise STATIC server/src/mdns_advertise.cpp)
    target_include_directories(sovd_mdns_advertise PUBLIC server/include)
    target_link_libraries(sovd_mdns_advertise PUBLIC ${AVAHI_CLIENT_LIBRARIES})
    target_include_directories(sovd_mdns_advertise PRIVATE ${AVAHI_CLIENT_INCLUDE_DIRS})
    target_compile_definitions(sovd_mdns_advertise PUBLIC SOVD_HAVE_MDNS=1)
endif()

add_executable(sovd_cli cli/src/main.cpp)
target_link_libraries(sovd_cli PRIVATE sovd_client)
if(SOVD_CLIENT_MDNS)
    target_compile_definitions(sovd_cli PRIVATE SOVD_HAVE_MDNS=1)
endif()

if(SOVD_CLIENT_MDNS)
    target_link_libraries(sovd_server PRIVATE sovd_mdns_advertise)
    target_compile_definitions(sovd_server PRIVATE SOVD_HAVE_MDNS=1)
endif()

# --- tests ---
enable_testing()
add_executable(test_core tests/test_core.cpp)
target_include_directories(test_core PRIVATE tests adapters/mock)
target_link_libraries(test_core PRIVATE sovd_server_lib sovd_config_loader sovd_catalog ${SOVD_ADAPTER_LIBS} Threads::Threads)
target_compile_definitions(test_core PRIVATE SOVD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME test_core COMMAND test_core)

# Live socket tests for adapters/uds_doip, built only alongside the adapter
# itself — the default (mock-only) build stays hermetic with no socket I/O.
if(SOVD_ADAPTER_UDS_DOIP)
    add_executable(test_uds_doip tests/test_uds_doip.cpp)
    target_include_directories(test_uds_doip PRIVATE tests third_party)
    target_link_libraries(test_uds_doip PRIVATE sovd_adapter_uds_doip sovd_server_lib Threads::Threads)
    target_compile_definitions(test_uds_doip PRIVATE SOVD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME test_uds_doip COMMAND test_uds_doip)
endif()

# Client SDK tests: real SovdClient against a real live sovd_server (mock
# topology), same "test the real path" standard as the rest of this repo.
add_executable(test_client tests/test_client.cpp)
target_include_directories(test_client PRIVATE tests adapters/mock)
target_link_libraries(test_client PRIVATE sovd_client sovd_server_lib sovd_catalog ${SOVD_ADAPTER_LIBS} Threads::Threads)
add_test(NAME test_client COMMAND test_client)
