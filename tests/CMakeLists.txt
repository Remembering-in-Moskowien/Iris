# Catch2 v3（内嵌 third_party/catch2，无 FetchContent）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/catch2 ${CMAKE_CURRENT_BINARY_DIR}/catch2)

add_executable(iris_tests
    test_time.cpp
    test_ass.cpp
    test_ir.cpp)
target_link_libraries(iris_tests PRIVATE iris_core Catch2::Catch2WithMain)
target_compile_options(iris_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

include(Catch)
catch_discover_tests(iris_tests)
