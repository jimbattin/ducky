# Host builds, no libDaisy: `make host` -> build/host/tests; `make test` builds and runs them.
HOST_DIR := build/host
HOST_CXXFLAGS := -std=gnu++17 -O2 -g -Wall -Wextra -Werror -Idaisy/src
GTEST_CFLAGS := $(shell pkg-config --cflags gtest_main)
GTEST_LIBS := $(shell pkg-config --libs gtest_main)
TEST_SOURCES := $(wildcard tests/*.cpp)
DSP_HEADERS := $(wildcard daisy/src/*.h daisy/src/dsp/*.h tests/*.h)

host: $(HOST_DIR)/tests

$(HOST_DIR)/tests: $(TEST_SOURCES) $(DSP_HEADERS) | $(HOST_DIR)
	$(CXX) $(HOST_CXXFLAGS) $(GTEST_CFLAGS) $(TEST_SOURCES) -o $@ $(GTEST_LIBS)

$(HOST_DIR):
	mkdir -p $@

test: host
	$(HOST_DIR)/tests

clean:
	rm -rf build

.PHONY: host test clean
