cmake_minimum_required(VERSION 3.16)
project(cpapdash_parser VERSION 2026.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(cpapdash_parser STATIC
    src/EDFFile.cpp
    src/EDFParser.cpp
    src/EDFParser_BRP.cpp
    src/EDFParser_PLD.cpp
    src/EDFParser_SAD.cpp
    src/EDFParser_EVE.cpp
    src/OximetryCsv.cpp
    src/EDFParser_STR.cpp
    src/ISessionParser.cpp
    src/Models.cpp
    src/DesatDetector.cpp
    src/VLDParser.cpp
    src/SleepIndex.cpp
)

target_include_directories(cpapdash_parser PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# std::filesystem: GCC < 9 and Clang < 9 need explicit linking
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(cpapdash_parser PUBLIC stdc++fs)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(cpapdash_parser PUBLIC c++fs)
endif()

# Lowenstein Prisma parser (optional, no extra dependencies)
option(CPAPDASH_PARSER_WITH_LOWENSTEIN "Build Lowenstein Prisma parser" OFF)

if(CPAPDASH_PARSER_WITH_LOWENSTEIN)
    target_sources(cpapdash_parser PRIVATE
        src/PrismaParser.cpp
        src/PrismaParser_Events.cpp
        src/PrismaParser_Signals.cpp
    )
    target_compile_definitions(cpapdash_parser PUBLIC CPAPDASH_WITH_LOWENSTEIN=1)
endif()

# Tests
option(CPAPDASH_PARSER_BUILD_TESTS "Build tests" OFF)
if(CPAPDASH_PARSER_BUILD_TESTS AND CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    enable_testing()
    find_package(GTest REQUIRED)

    set(TEST_SOURCES
        tests/test_edf_file.cpp
        tests/test_brp_checkpoints.cpp
        tests/test_eve_multiblock.cpp
        tests/test_event_classification.cpp
        tests/test_models.cpp
        tests/test_vld_parser.cpp
        tests/test_o2ring_csv.cpp
        tests/test_str_family.cpp
        tests/test_desat_detector.cpp
        tests/test_detect_manufacturer.cpp
        tests/test_sleep_index.cpp
        tests/test_event_indexes.cpp
        tests/test_breath_analysis.cpp
    )
    if(CPAPDASH_PARSER_WITH_LOWENSTEIN)
        list(APPEND TEST_SOURCES tests/test_prisma.cpp)
        list(APPEND TEST_SOURCES tests/test_prisma_channels.cpp)
    endif()

    add_executable(run_tests ${TEST_SOURCES})
    target_link_libraries(run_tests cpapdash_parser GTest::GTest GTest::Main)
    # ctest runs the binary from the BUILD directory while a developer running
    # ./run_tests by hand is usually at the repository root, so a fixture path
    # relative to either one alone passes in one place and fails in the other.
    target_compile_definitions(run_tests PRIVATE
        CPAPDASH_PARSER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME parser_tests COMMAND run_tests)
endif()
