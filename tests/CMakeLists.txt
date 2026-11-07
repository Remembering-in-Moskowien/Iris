# Catch2 v3（内嵌 third_party/catch2，无 FetchContent）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/catch2 ${CMAKE_CURRENT_BINARY_DIR}/catch2)

add_executable(iris_tests
    test_time.cpp
    test_ass.cpp
    test_ir.cpp
    test_lint.cpp
    test_build.cpp
    test_karaoke.cpp
    test_merge.cpp
    test_rules.cpp
    test_align.cpp
    test_project.cpp
    test_extension_registry.cpp
    test_pipeline_manifest.cpp
    test_process_plugin.cpp
    test_tts.cpp)
target_link_libraries(iris_tests PRIVATE iris_core Catch2::Catch2WithMain)
target_compile_options(iris_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# T3 进程插件测试用的最小插件（只依赖 nlohmann/json，避免拖入推理运行时）。
add_executable(iris_mock_plugin mock_plugin.cpp)
target_include_directories(iris_mock_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/json)
target_compile_options(iris_mock_plugin PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(iris_mock_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_dependencies(iris_tests iris_mock_plugin)

include(Catch)
# 依赖 models/ 的用例用仓库根目录相对路径，ctest 默认在 build/tests 下跑会静默跳过。
catch_discover_tests(iris_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
