# Host builds, no libDaisy: `make host` -> build/host/tests and build/host/render; `make test` builds and runs the tests.
HOST_DIR := build/host
HOST_CXXFLAGS := -std=gnu++17 -O2 -g -Wall -Wextra -Werror -Idaisy/src -Itools
GTEST_CFLAGS := $(shell pkg-config --cflags gtest_main)
GTEST_LIBS := $(shell pkg-config --libs gtest_main)
TEST_SOURCES := $(wildcard tests/*.cpp)
DSP_HEADERS := $(wildcard daisy/src/*.h daisy/src/dsp/*.h tests/*.h tools/*.h)

host: $(HOST_DIR)/tests $(HOST_DIR)/render

$(HOST_DIR)/tests: $(TEST_SOURCES) $(DSP_HEADERS) | $(HOST_DIR)
	$(CXX) $(HOST_CXXFLAGS) $(GTEST_CFLAGS) -DDK_REPO_ROOT='"$(CURDIR)"' $(TEST_SOURCES) -o $@ $(GTEST_LIBS)

$(HOST_DIR)/render: tools/render.cpp $(DSP_HEADERS) | $(HOST_DIR)
	$(CXX) $(HOST_CXXFLAGS) tools/render.cpp -o $@

render: $(HOST_DIR)/render

$(HOST_DIR):
	mkdir -p $@

test: host
	$(HOST_DIR)/tests

clean:
	rm -rf build

.PHONY: host render test clean
