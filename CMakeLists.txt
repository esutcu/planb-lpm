cmake_minimum_required(VERSION 3.14)
project(planb-lpm CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(LPM6_BUILD_PYTHON "Build the pybind11 Python extension" OFF)

# Detect AVX-512 so we don't force flags on ARM / older x86.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-mavx512f"  LPM6_CAN_AVX512F)
check_cxx_compiler_flag("-mavx512dq" LPM6_CAN_AVX512DQ)

add_library(lpm6 INTERFACE)
target_include_directories(lpm6 INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features   (lpm6 INTERFACE cxx_std_17)
if(LPM6_CAN_AVX512F AND LPM6_CAN_AVX512DQ)
    target_compile_options(lpm6 INTERFACE -mavx512f -mavx512dq)
endif()

# Release flags: mild tuning, no -march=native by default so we keep portable
# binaries.  Users can override on the command line.
if(NOT MSVC)
    target_compile_options(lpm6 INTERFACE $<$<CONFIG:Release>:-O3>)
endif()

add_executable(planb-lpm src/main.cpp)
target_link_libraries(planb-lpm PRIVATE lpm6)
if(WIN32)
    target_link_libraries(planb-lpm PRIVATE ws2_32)
endif()

enable_testing()
add_executable(test_correctness tests/test_correctness.cpp)
target_link_libraries(test_correctness PRIVATE lpm6)
if(WIN32)
    target_link_libraries(test_correctness PRIVATE ws2_32)
endif()
add_test(NAME correctness COMMAND test_correctness)

add_executable(bench_naive tests/bench_naive.cpp)
target_link_libraries(bench_naive PRIVATE lpm6)
if(WIN32)
    target_link_libraries(bench_naive PRIVATE ws2_32)
endif()

add_executable(bench_update tests/bench_update.cpp)
target_link_libraries(bench_update PRIVATE lpm6)
if(WIN32)
    target_link_libraries(bench_update PRIVATE ws2_32)
endif()

add_executable(bench_mt tests/bench_mt.cpp)
target_link_libraries(bench_mt PRIVATE lpm6)
find_package(Threads REQUIRED)
target_link_libraries(bench_mt PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(bench_mt PRIVATE ws2_32)
endif()

if(LPM6_BUILD_PYTHON)
    find_package(pybind11 CONFIG REQUIRED)
    pybind11_add_module(planb_lpm python/binding.cpp)
    target_link_libraries(planb_lpm PRIVATE lpm6)
    if(WIN32)
        target_link_libraries(planb_lpm PRIVATE ws2_32)
    endif()
    # scikit-build-core picks up installed targets for the wheel.
    install(TARGETS planb_lpm LIBRARY DESTINATION . COMPONENT python)
endif()

message(STATUS "build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "AVX-512         : ${LPM6_CAN_AVX512F}/${LPM6_CAN_AVX512DQ}")
message(STATUS "python binding  : ${LPM6_BUILD_PYTHON}")
