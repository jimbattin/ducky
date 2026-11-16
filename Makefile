# Host builds, no libDaisy: `make host` -> build/host/tests and build/host/render; `make test` builds and runs the tests.
include daisy/cmsis_fft.mk

HOST_DIR := build/host
# CMSIS-DSP's own host-build switch (__GNUC_PYTHON__): plain C, no Arm intrinsics.
CMSIS_DIR := daisy/libDaisy/Drivers/CMSIS-DSP
HOST_CXXFLAGS := -std=gnu++17 -O2 -g -Wall -Wextra -Werror -Idaisy/src -Itools -D__GNUC_PYTHON__ -I$(CMSIS_DIR)/Include
GTEST_CFLAGS := $(shell pkg-config --cflags gtest_main)
GTEST_LIBS := $(shell pkg-config --libs gtest_main)
TEST_SOURCES := $(wildcard tests/*.cpp)
DSP_HEADERS := $(wildcard daisy/src/*.h daisy/src/dsp/*.h tests/*.h tools/*.h)
HOST_CMSIS_OBJS := $(addprefix $(HOST_DIR)/cmsis/,$(notdir $(CMSIS_FFT_FILES:.c=.o)))
vpath %.c $(CMSIS_DIR)/Source/TransformFunctions $(CMSIS_DIR)/Source/CommonTables

host: $(HOST_DIR)/tests $(HOST_DIR)/render

$(HOST_DIR)/tests: $(TEST_SOURCES) $(DSP_HEADERS) $(HOST_CMSIS_OBJS) | $(HOST_DIR)
	$(CXX) $(HOST_CXXFLAGS) $(GTEST_CFLAGS) -DDK_REPO_ROOT='"$(CURDIR)"' $(TEST_SOURCES) $(HOST_CMSIS_OBJS) -o $@ $(GTEST_LIBS)

$(HOST_DIR)/render: tools/render.cpp $(DSP_HEADERS) $(HOST_CMSIS_OBJS) | $(HOST_DIR)
	$(CXX) $(HOST_CXXFLAGS) tools/render.cpp $(HOST_CMSIS_OBJS) -o $@

# Third-party C: no -Werror, and $(CC) (g++ would compile .c as C++).
$(HOST_DIR)/cmsis/%.o: %.c daisy/cmsis_fft.mk | $(HOST_DIR)/cmsis
	$(CC) -std=gnu11 -O2 -D__GNUC_PYTHON__ $(CMSIS_FFT_DEFS) -I$(CMSIS_DIR)/Include -I$(CMSIS_DIR)/PrivateInclude -c $< -o $@

render: $(HOST_DIR)/render

$(HOST_DIR) $(HOST_DIR)/cmsis:
	mkdir -p $@

test: host
	$(HOST_DIR)/tests

clean:
	rm -rf build

.PHONY: host render test clean
