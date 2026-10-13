CXX ?= g++
CXXFLAGS ?= -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic

tail_bench: src/benchmark.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

test: tail_bench
	python3 tests/test_benchmark.py

clean:
	rm -f tail_bench

.PHONY: test clean
