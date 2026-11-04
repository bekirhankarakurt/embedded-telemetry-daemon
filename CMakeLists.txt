cmake_minimum_required(VERSION 3.16)

project(embedded-telemetry-daemon
    VERSION 1.0.0
    DESCRIPTION "Lightweight Linux telemetry recorder with UDP downlink"
    LANGUAGES CXX)

option(ETD_WERROR "Treat compiler warnings as errors" OFF)
option(ETD_SANITIZE_ADDRESS "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(ETD_SANITIZE_THREAD "Build with ThreadSanitizer" OFF)
option(ETD_BUILD_TESTS "Build unit tests" ON)

if(ETD_SANITIZE_ADDRESS AND ETD_SANITIZE_THREAD)
    message(FATAL_ERROR "ETD_SANITIZE_ADDRESS and ETD_SANITIZE_THREAD are mutually exclusive")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
include(GNUInstallDirs)

# ---- Common compile options -------------------------------------------------
add_library(etd_options INTERFACE)
target_compile_options(etd_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual
    $<$<BOOL:${ETD_WERROR}>:-Werror>)
target_compile_definitions(etd_options INTERFACE ETD_VERSION="${PROJECT_VERSION}")

if(ETD_SANITIZE_ADDRESS)
    target_compile_options(etd_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(etd_options INTERFACE -fsanitize=address,undefined)
elseif(ETD_SANITIZE_THREAD)
    target_compile_options(etd_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(etd_options INTERFACE -fsanitize=thread)
endif()

# ---- Core library shared by daemon, listener and tests ----------------------
add_library(etd_core STATIC
    src/telemetry_data.cpp
    src/collector.cpp
    src/udp_sender.cpp
    src/rolling_logger.cpp)
target_include_directories(etd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(etd_core PUBLIC etd_options Threads::Threads)

# ---- Executables ------------------------------------------------------------
add_executable(embedded-telemetry-daemon src/main.cpp)
target_link_libraries(embedded-telemetry-daemon PRIVATE etd_core)

add_executable(telemetry_listener tools/telemetry_listener.cpp)
target_link_libraries(telemetry_listener PRIVATE etd_core)

# ---- Tests ------------------------------------------------------------------
if(ETD_BUILD_TESTS)
    enable_testing()
    foreach(t parsers wire_format bounded_queue rolling_logger udp_sender collector)
        add_executable(test_${t} tests/test_${t}.cpp)
        target_link_libraries(test_${t} PRIVATE etd_core)
        add_test(NAME ${t} COMMAND test_${t})
        set_tests_properties(${t} PROPERTIES TIMEOUT 30)
    endforeach()
    target_compile_definitions(test_parsers PRIVATE ETD_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
endif()

# ---- Install ----------------------------------------------------------------
install(TARGETS embedded-telemetry-daemon telemetry_listener RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES systemd/embedded-telemetry-daemon.service DESTINATION lib/systemd/system)
