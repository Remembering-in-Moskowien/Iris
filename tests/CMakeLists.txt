# Catch2 v3 test framework (fetched on demand)
include(FetchContent)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.6.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2)

add_executable(daisy_tests
    test_time.cpp
    test_ass.cpp
    test_ir.cpp)
target_link_libraries(daisy_tests PRIVATE daisy_core Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(daisy_tests)
