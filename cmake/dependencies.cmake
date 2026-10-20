# ============================================================================
# Iris 依赖策略（DLL 运行时 + 推理代码自写）
#
# ASR 推理运行时以「打包好的 DLL」引用，不内嵌源码：
#   - tools/runtimes/crispasr  CrispASR DLL（whisper ggml / qwen3-asr ggml 运行时）
#   - tools/runtimes/ct2       CTranslate2 DLL（whisper transformer 运行时）
#   - tools/runtimes/onnx      onnxruntime DLL（whisper ONNX，GPU 可行，可选）
# 推理编排代码（mel 提取、窗口化、token 解析、prompt 构造）由 Iris 自写（src/asr/）。
#
# 构建期依赖（非推理运行时）以内嵌单头/小库形式提供：
#   - third_party/json   nlohmann_json v3.11.3
#   - third_party/cli11  CLI11 v2.4.2
#   - third_party/spdlog v1.14.1
#   - third_party/catch2 Catch2 v3.6.0（仅测试）
#
# 保留的外部运行时工具（子进程，不属于 ASR 推理）：ffmpeg（媒体转码）、VSF（可选人声分离）。
# ============================================================================

# ---- 单头库：nlohmann_json / CLI11 ----------------------------------------
add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json)

add_library(CLI11 INTERFACE)
add_library(CLI11::CLI11 ALIAS CLI11)
target_include_directories(CLI11 INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cli11)

# ---- spdlog（内嵌，v1.14.1）-----------------------------------------------
if(NOT TARGET spdlog)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog EXCLUDE_FROM_ALL)
endif()

# ---- 打包好的运行时 DLL（tools/runtimes，gitignored，不内嵌源码）------------
set(IRIS_RUNTIMES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/runtimes" CACHE PATH
    "Directory containing packaged ASR runtime DLLs (crispasr/ct2/onnx)")

# CrispASR DLL —— whisper(ggml) / qwen3-asr(ggml) 推理运行时。
if(EXISTS "${IRIS_RUNTIMES_DIR}/crispasr/crispasr.lib" AND
   EXISTS "${IRIS_RUNTIMES_DIR}/crispasr/include/crispasr_session.h")
    add_library(iris_crispasr SHARED IMPORTED)
    set_target_properties(iris_crispasr PROPERTIES
        IMPORTED_IMPLIB "${IRIS_RUNTIMES_DIR}/crispasr/crispasr.lib"
        IMPORTED_LOCATION "${IRIS_RUNTIMES_DIR}/crispasr/crispasr.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${IRIS_RUNTIMES_DIR}/crispasr/include")
    set(IRIS_HAS_CRISPASR ON)
    message(STATUS "Iris: CrispASR runtime DLL found at tools/runtimes/crispasr")
else()
    set(IRIS_HAS_CRISPASR OFF)
    message(STATUS "Iris: CrispASR runtime DLL NOT found (tools/runtimes/crispasr) — ggml 后端不可用")
endif()

# CTranslate2 DLL —— whisper(transformer) 推理运行时。
if(EXISTS "${IRIS_RUNTIMES_DIR}/ct2/ctranslate2.lib" AND
   EXISTS "${IRIS_RUNTIMES_DIR}/ct2/include/ctranslate2/models/whisper.h")
    add_library(iris_ct2 SHARED IMPORTED)
    set_target_properties(iris_ct2 PROPERTIES
        IMPORTED_IMPLIB "${IRIS_RUNTIMES_DIR}/ct2/ctranslate2.lib"
        IMPORTED_LOCATION "${IRIS_RUNTIMES_DIR}/ct2/ctranslate2.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${IRIS_RUNTIMES_DIR}/ct2/include")
    set(IRIS_HAS_CT2 ON)
    message(STATUS "Iris: CTranslate2 runtime DLL found at tools/runtimes/ct2")
else()
    set(IRIS_HAS_CT2 OFF)
    message(STATUS "Iris: CTranslate2 runtime DLL NOT found (tools/runtimes/ct2) — whisper-ct2 后端不可用")
endif()

# onnxruntime DLL —— whisper(ONNX) 推理运行时（GPU 可行：CUDA EP）。
if(EXISTS "${IRIS_RUNTIMES_DIR}/onnx/onnxruntime.lib" AND
   EXISTS "${IRIS_RUNTIMES_DIR}/onnx/include/onnxruntime_cxx_api.h")
    add_library(iris_onnx SHARED IMPORTED)
    set_target_properties(iris_onnx PROPERTIES
        IMPORTED_IMPLIB "${IRIS_RUNTIMES_DIR}/onnx/onnxruntime.lib"
        IMPORTED_LOCATION "${IRIS_RUNTIMES_DIR}/onnx/onnxruntime.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${IRIS_RUNTIMES_DIR}/onnx/include")
    set(IRIS_HAS_ONNX ON)
    message(STATUS "Iris: onnxruntime DLL found at tools/runtimes/onnx")
else()
    set(IRIS_HAS_ONNX OFF)
    message(STATUS "Iris: onnxruntime DLL NOT found (tools/runtimes/onnx) — whisper-onnx 后端不可用")
endif()
