cmake_minimum_required(VERSION 3.20)
project(daisy VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(DAISY_BUILD_TESTS "Build the test suite" ON)

include(cmake/dependencies.cmake)

# ---------------------------------------------------------------------------
# Core library: models + engine + operators + infrastructure
# ---------------------------------------------------------------------------
add_library(daisy_core
    src/core/pipeline.cpp
    src/core/provider.cpp
    src/model/config.cpp
    src/model/time.cpp
    src/model/ir_document.cpp
    src/model/subtitle.cpp
    src/ops/ffmpeg_convert.cpp
    src/ops/audio_preprocess.cpp
    src/ops/transcribe.cpp
    src/ops/sentence_split.cpp
    src/ops/ass_build.cpp
    src/infra/process.cpp
    src/providers/asr.cpp)
target_include_directories(daisy_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(daisy_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)
target_link_libraries(daisy_core PUBLIC
    nlohmann_json::nlohmann_json
    spdlog::spdlog)

# ---------------------------------------------------------------------------
# CLI executable
# ---------------------------------------------------------------------------
add_executable(daisy
    src/main.cpp
    src/cli/cli.cpp)
target_link_libraries(daisy PRIVATE daisy_core CLI11::CLI11)
set_target_properties(daisy PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(DAISY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
