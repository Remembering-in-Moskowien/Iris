cmake_minimum_required(VERSION 3.24)
project(iris VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 鍏ㄩ」鐩潤鎬?CRT锛?MT锛夛紝涓庡唴宓?CT2 闈欐€佸簱鐨勮繍琛屾椂涓€鑷达紝閬垮厤 LNK2038銆?if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC runtime library" FORCE)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(IRIS_BUILD_TESTS "Build the test suite" ON)

# 鏋勫缓鍙凤細CI 娉ㄥ叆 GitHub run number锛況elease tag = v{run_number}锛?-version 鍚屾鏄剧ず銆?set(IRIS_BUILD_NUMBER "" CACHE STRING "Build number appended to version (CI run number)")
if(IRIS_BUILD_NUMBER)
  set(IRIS_VERSION_STRING "0.1.${IRIS_BUILD_NUMBER}")
else()
  set(IRIS_VERSION_STRING "0.1.0")
endif()
add_compile_definitions(IRIS_VERSION_STRING="${IRIS_VERSION_STRING}")
message(STATUS "Iris: version ${IRIS_VERSION_STRING}")

include(cmake/dependencies.cmake)

# ---------------------------------------------------------------------------
# MVC 鍒嗗眰锛圡odel-View-Controller锛夛細
#   model/      M 鈥?鏁版嵁妯″瀷涓?IR 濂戠害
#   view/       V 鈥?瀛楀箷娓叉煋锛圓SS/SRT/TXT锛変笌鎺у埗鍙板憟鐜?#   controller/ C 鈥?鍛戒护澶勭悊鍣紙asr/build/validate/doctor/models锛?#   core/      寮曟搸锛氱绾?鎿嶄綔绗?鎻愪緵鑰呮敞鍐岃〃锛圕ontroller 涓?ops 鐨勫簳搴э級
#   ops/       涓氬姟鎿嶄綔绗︼紙鏈嶅姟灞傦細杞爜/棰勫鐞?杞啓/鍒嗗彞锛?#   asr/       ASR 妯″潡锛氳繘绋嬪唴鎺ㄧ悊鍚庣锛圕rispASR ggml / CTranslate2锛?#   infra/     鍩虹璁炬柦锛堟棩蹇椼€佸瓙杩涚▼锛?# ---------------------------------------------------------------------------
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

# ASR 鎺ㄧ悊鍚庣锛氳繍琛屾椂浠ユ墦鍖?DLL 寮曠敤锛坱ools/runtimes锛屼笉鍐呭祵婧愮爜锛夈€?if(IRIS_HAS_CRISPASR)
    target_link_libraries(iris_core PUBLIC iris_crispasr)
endif()
if(IRIS_HAS_CT2)
    target_link_libraries(iris_core PUBLIC iris_ct2)
endif()
if(IRIS_HAS_ONNX)
    target_link_libraries(iris_core PUBLIC iris_onnx)
endif()

# ---------------------------------------------------------------------------
# CLI executable锛堣杽澹筹細CLI11 瑙ｆ瀽 鈫?鍒嗗彂鍒?controller/锛?# ---------------------------------------------------------------------------
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
