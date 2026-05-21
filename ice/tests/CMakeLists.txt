cmake_minimum_required(VERSION 3.14)

include(FetchContent)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)
# FetchContent_MakeAvailable 会下载并调用 add_subdirectory(googletest)
# 这使得 GTest::gtest_main 等目标在我们的项目中可用
FetchContent_MakeAvailable(googletest)

# ICE test executable
add_executable(ice_unitest ice_unitest.cpp)

target_link_libraries(ice_unitest PRIVATE
    gtest_main
    ice
)

if(WIN32)
    target_link_libraries(ice_unitest PRIVATE iphlpapi ws2_32 secur32 crypt32)
endif()

# Register with CTest
enable_testing()
include(GoogleTest)
gtest_discover_tests(ice_unitest)

# add_test(NAME ice_unitest COMMAND ice_unitest)
# Custom target for convenience
# add_custom_target(run_ice_tests DEPENDS ice_unitest)
