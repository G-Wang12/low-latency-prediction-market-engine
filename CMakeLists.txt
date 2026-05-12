cmake_minimum_required(VERSION 3.20)

#+------------------------------------------------------------------------------+
# Project
#+------------------------------------------------------------------------------+
# Note: don't force CMAKE_OSX_ARCHITECTURES here. It breaks Intel Macs and can
# conflict with user/toolchain settings. If you need a specific arch, pass:
#   -DCMAKE_OSX_ARCHITECTURES=arm64 (or x86_64)

# Conda frequently injects CMake search paths that can cause mixed Boost versions
# (e.g., Homebrew BoostConfig + conda boost_system). Prefer native system deps.
option(LLPME_IGNORE_CONDA_PREFIX "Ignore CONDA_PREFIX when searching for packages" ON)
if(LLPME_IGNORE_CONDA_PREFIX AND DEFINED ENV{CONDA_PREFIX})
    list(APPEND CMAKE_IGNORE_PREFIX_PATH "$ENV{CONDA_PREFIX}")
endif()

project(LowLatencyPredictionMarketEngine
    VERSION 0.1.0
    DESCRIPTION "Low-latency C++ execution engine for prediction markets"
    LANGUAGES CXX
)

# Default build type for single-config generators (Ninja/Makefiles)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict compiler warnings + Release optimizations (apply to OUR targets only).
function(llpme_target_strict_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        # Newer AppleClang/Clang can emit warnings from third-party headers that become build
        # breaks under -Werror (e.g. simdjson's deprecated literal-operator form).
        # Keep -Werror for our code, but don't hard-fail on these external-header diagnostics.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE -Wno-error=deprecated-literal-operator)
        endif()
    endif()
endfunction()

function(llpme_target_release_opts target)
    if(MSVC)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/O2>)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # Requirement: -O3 -march=native in Release builds
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-O3;-march=native>)
    endif()
endfunction()

function(llpme_target_link_boost target)
    # Boost can be provided either via CMake's FindBoost module (classic) or via
    # Boost's own CMake packages (component packages like boost_system).
    if(TARGET Boost::system AND TARGET Boost::thread)
        target_link_libraries(${target} PRIVATE Boost::system Boost::thread)
        return()
    endif()

    if(TARGET boost_system AND TARGET boost_thread)
        target_link_libraries(${target} PRIVATE boost_system boost_thread)
        return()
    endif()

    if(Boost_INCLUDE_DIRS)
        target_include_directories(${target} PRIVATE ${Boost_INCLUDE_DIRS})
    endif()

    if(DEFINED Boost_SYSTEM_LIBRARY AND DEFINED Boost_THREAD_LIBRARY)
        target_link_libraries(${target} PRIVATE ${Boost_SYSTEM_LIBRARY} ${Boost_THREAD_LIBRARY})
        return()
    endif()

    if(Boost_LIBRARIES)
        target_link_libraries(${target} PRIVATE ${Boost_LIBRARIES})
        return()
    endif()

    message(FATAL_ERROR "Boost was found, but no usable imported targets or libraries were exposed.")
endfunction()

# Enable position independent code for libraries
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# FetchContent for dependencies
include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# simdjson for zero-allocation JSON parsing (critical for low-latency market data)
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG tags/v3.6.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(simdjson)

# GoogleTest for unit testing - configure BEFORE FetchContent_MakeAvailable
# to prevent debug flags (-Og) leaking into Release builds
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(gtest_disable_pthreads OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

# Boost - assume installed locally via system package manager
# Requires system and thread components for networking, async, etc.
find_package(Boost CONFIG QUIET COMPONENTS system thread)
if(NOT Boost_FOUND)
    # Fallback for environments where Boost doesn't ship CMake config files.
    find_package(Boost REQUIRED COMPONENTS system thread)
endif()
if(NOT Boost_FOUND)
    message(FATAL_ERROR "Boost not found. Please install Boost with system and thread components.")
endif()

# OpenSSL is required for wss:// (TLS) via boost::asio::ssl + boost::beast::ssl_stream
find_package(OpenSSL REQUIRED)

# Main executable
add_executable(engine
    src/main.cpp
    src/websocket_client.cpp
    src/strategy_engine.cpp
)

target_compile_features(engine PRIVATE cxx_std_20)

target_include_directories(engine
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(engine
    PRIVATE
        simdjson
    OpenSSL::SSL
    OpenSSL::Crypto
)

llpme_target_link_boost(engine)

target_compile_definitions(engine
    PRIVATE
        BOOST_ASIO_NO_DEPRECATED
)

llpme_target_strict_warnings(engine)
llpme_target_release_opts(engine)

# Tests
enable_testing()

add_executable(tests
    tests/test_main.cpp
)

target_compile_features(tests PRIVATE cxx_std_20)

target_include_directories(tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Link only gtest_main (includes gtest); avoids duplicate static lib warnings
# that cause missing MakeAndRegisterTestInfo symbol on macOS/x86_64
target_link_libraries(tests
    PRIVATE
        GTest::gtest_main
        simdjson
)

llpme_target_link_boost(tests)

llpme_target_strict_warnings(tests)
llpme_target_release_opts(tests)

include(GoogleTest)
gtest_discover_tests(tests)

# Installation
install(TARGETS engine DESTINATION bin)

# Print configuration summary
message(STATUS "Low Latency Prediction Market Engine configured")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  simdjson: via FetchContent (v3.6.3)")
message(STATUS "  GoogleTest: via FetchContent (v1.15.2)")
message(STATUS "  Boost: ${Boost_VERSION} (system, thread)")
