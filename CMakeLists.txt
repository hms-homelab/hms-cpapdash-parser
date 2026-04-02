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
    src/Models.cpp
)

target_include_directories(sleeplink_parser PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Tests
option(SLEEPLINK_PARSER_BUILD_TESTS "Build tests" OFF)
if(SLEEPLINK_PARSER_BUILD_TESTS AND CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    enable_testing()
    find_package(GTest REQUIRED)

    add_executable(run_tests
        tests/test_edf_file.cpp
        tests/test_models.cpp
    )
    target_link_libraries(run_tests sleeplink_parser GTest::GTest GTest::Main)
    add_test(NAME parser_tests COMMAND run_tests)
endif()
