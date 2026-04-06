cmake_minimum_required(VERSION 3.16)
project(sleeplink_parser VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(sleeplink_parser STATIC
    src/EDFFile.cpp
    src/EDFParser.cpp
    src/EDFParser_BRP.cpp
    src/EDFParser_PLD.cpp
    src/EDFParser_SAD.cpp
    src/EDFParser_EVE.cpp
    src/EDFParser_STR.cpp
    src/ISessionParser.cpp
    src/Models.cpp
)

target_include_directories(sleeplink_parser PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# std::filesystem: GCC < 9 and Clang < 9 need explicit linking
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(sleeplink_parser PUBLIC stdc++fs)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(sleeplink_parser PUBLIC c++fs)
endif()

# Philips DreamStation 2 parser (optional, requires OpenSSL for AES-256-GCM decryption)
option(SLEEPLINK_PARSER_WITH_PHILIPS "Build Philips DreamStation parser (requires OpenSSL)" OFF)

if(SLEEPLINK_PARSER_WITH_PHILIPS)
    find_package(OpenSSL REQUIRED)
    target_sources(sleeplink_parser PRIVATE
        src/PhilipsCrypto.cpp
        src/PhilipsChunk.cpp
        src/PhilipsParser.cpp
        src/PhilipsParser_Summary.cpp
        src/PhilipsParser_Events.cpp
    )
    target_link_libraries(sleeplink_parser PUBLIC OpenSSL::Crypto)
    target_compile_definitions(sleeplink_parser PUBLIC SLEEPLINK_WITH_PHILIPS=1)
endif()

# Tests
option(SLEEPLINK_PARSER_BUILD_TESTS "Build tests" OFF)
if(SLEEPLINK_PARSER_BUILD_TESTS AND CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    enable_testing()
    find_package(GTest REQUIRED)

    add_executable(run_tests
        tests/test_edf_file.cpp
        tests/test_models.cpp
        tests/test_philips.cpp
    )
    target_link_libraries(run_tests sleeplink_parser GTest::GTest GTest::Main)
    add_test(NAME parser_tests COMMAND run_tests)
endif()
