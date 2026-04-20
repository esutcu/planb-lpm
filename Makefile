# Convenience Makefile for quick local builds.  The primary build system is
# CMake (see CMakeLists.txt) which also builds tests and Python bindings.

CXX      ?= g++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra
SIMD     ?= -mavx512f -mavx512dq

INCLUDE  = -Iinclude -Isrc

all: planb-lpm test_correctness bench_naive

planb-lpm: src/main.cpp include/lpm6.hpp src/ipv6_util.hpp
	$(CXX) $(CXXFLAGS) $(SIMD) $(INCLUDE) -o $@ src/main.cpp

test_correctness: tests/test_correctness.cpp include/lpm6.hpp src/ipv6_util.hpp
	$(CXX) $(CXXFLAGS) $(SIMD) $(INCLUDE) -o $@ tests/test_correctness.cpp

bench_naive: tests/bench_naive.cpp include/lpm6.hpp src/ipv6_util.hpp
	$(CXX) $(CXXFLAGS) $(SIMD) $(INCLUDE) -o $@ tests/bench_naive.cpp

test: test_correctness
	./test_correctness

clean:
	rm -f planb-lpm test_correctness bench_naive

.PHONY: all test clean
