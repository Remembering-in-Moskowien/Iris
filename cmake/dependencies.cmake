# Third-party dependencies, fetched at configure time.
# daisy uses the same strategy as the C# original: minimal vendoring,
# dependencies resolved by the build system.
include(FetchContent)

# --- nlohmann/json: JSON model serialization (IR *.daisy.json) ---
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# --- spdlog: structured logging ---
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH   OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_SHARED  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(spdlog)

# --- CLI11: command-line parsing (Spectre.Console equivalent in C#) ---
FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.2
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(CLI11)
