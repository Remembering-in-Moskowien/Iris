cmake_minimum_required(VERSION 3.24)
project(iris VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 全项目静态 CRT（/MT），与内嵌 CT2 静态库的运行时一致，避免 LNK2038。
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC runtime library" FORCE)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(IRIS_BUILD_TESTS "Build the test suite" ON)

# 构建号：CI 注入 GitHub run number；release tag = v{run_number}，--version 同步显示。
set(IRIS_BUILD_NUMBER "" CACHE STRING "Build number appended to version (CI run number)")
if(IRIS_BUILD_NUMBER)
  set(IRIS_VERSION_STRING "0.1.${IRIS_BUILD_NUMBER}")
else()
  set(IRIS_VERSION_STRING "0.1.0")
endif()
add_compile_definitions(IRIS_VERSION_STRING="${IRIS_VERSION_STRING}")
message(STATUS "Iris: version ${IRIS_VERSION_STRING}")

include(cmake/dependencies.cmake)

# ---------------------------------------------------------------------------
# MVC 分层（Model-View-Controller）：
#   model/      M — 数据模型与 IR 契约
#   view/       V — 字幕渲染（ASS/SRT/TXT）与控制台呈现
#   controller/ C — 命令处理器（asr/build/validate/doctor/models）
#   core/      引擎：管线/操作符/提供者注册表（Controller 与 ops 的底座）
#   ops/       业务操作符（服务层：转码/预处理/转写/分句）
#   asr/       ASR 模块：进程内推理后端（CrispASR ggml / CTranslate2）
#   infra/     基础设施（日志、子进程）
# ---------------------------------------------------------------------------
add_library(iris_core
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
    src/asr/asr.cpp
    src/asr/ct2_mel.cpp
    src/asr/onnx.cpp
    src/view/subtitle_renderer.cpp
    src/infra/process.cpp)
target_include_directories(iris_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(iris_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)
target_compile_definitions(iris_core PUBLIC
    IRIS_HAS_CRISPASR=$<BOOL:${IRIS_HAS_CRISPASR}>
    IRIS_HAS_CT2=$<BOOL:${IRIS_HAS_CT2}>
    IRIS_HAS_ONNX=$<BOOL:${IRIS_HAS_ONNX}>)
target_link_libraries(iris_core PUBLIC
    nlohmann_json::nlohmann_json
    spdlog::spdlog)

# ASR 推理后端：运行时以打包 DLL 引用（tools/runtimes，不内嵌源码）。
if(IRIS_HAS_CRISPASR)
    target_link_libraries(iris_core PUBLIC iris_crispasr)
endif()
if(IRIS_HAS_CT2)
    target_link_libraries(iris_core PUBLIC iris_ct2)
endif()
if(IRIS_HAS_ONNX)
    target_link_libraries(iris_core PUBLIC iris_onnx)
endif()

# ---------------------------------------------------------------------------
# CLI executable（薄壳：CLI11 解析 → 分发到 controller/）
# ---------------------------------------------------------------------------
add_executable(iris
    src/main.cpp
    src/cli/cli.cpp
    src/controller/asr_command.cpp
    src/controller/build_command.cpp
    src/controller/validate_command.cpp
    src/controller/doctor_command.cpp
    src/controller/models_command.cpp)
target_link_libraries(iris PRIVATE iris_core CLI11::CLI11)
target_compile_options(iris PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(iris PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(IRIS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
