# Host builds, no libDaisy: `make host` -> build/host/tests and build/host/render; `make test` runs the tests and
# checks docs/MIDI.md is current; `make midi-doc` regenerates docs/MIDI.md (tools/gen_midi_doc.cpp); `make morph-check`
# checks tools/morph_train.py against the fixtures; `make firmware-check` checks the firmware update on the pedal;
# `make clean` removes the host and firmware builds.
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

$(HOST_DIR)/gen_midi_doc: tools/gen_midi_doc.cpp $(DSP_HEADERS) | $(HOST_DIR)
	$(CXX) $(HOST_CXXFLAGS) tools/gen_midi_doc.cpp -o $@

# Third-party C: no -Werror, and $(CC) (g++ would compile .c as C++).
$(HOST_DIR)/cmsis/%.o: %.c daisy/cmsis_fft.mk | $(HOST_DIR)/cmsis
	$(CC) -std=gnu11 -O2 -D__GNUC_PYTHON__ $(CMSIS_FFT_DEFS) -I$(CMSIS_DIR)/Include -I$(CMSIS_DIR)/PrivateInclude -c $< -o $@

render: $(HOST_DIR)/render

$(HOST_DIR) $(HOST_DIR)/cmsis:
	mkdir -p $@

test: host check-midi-doc
	$(HOST_DIR)/tests

midi-doc: $(HOST_DIR)/gen_midi_doc
	$(HOST_DIR)/gen_midi_doc > docs/MIDI.md.tmp && mv docs/MIDI.md.tmp docs/MIDI.md

# Fails when docs/MIDI.md is not what `make midi-doc` would write.
check-midi-doc: $(HOST_DIR)/gen_midi_doc
	$(HOST_DIR)/gen_midi_doc | diff -u docs/MIDI.md -

# The morph path trainer's network and fit against tests/fixtures (uv; the first run downloads PyTorch).
morph-check:
	uv run tools/morph_train.py --check

# The firmware update over USB-MIDI on the attached pedal, unattended (tools/firmware_check.ts): needs the pedal on
# DK_USB_PORT running Ducky, its MIDI port free (no editor tab, no tools/usb_log.py), Chrome and an empty slot. It
# reflashes the pedal several times and ends on the working tree's xfade image. BOARD=burningbungalow selects the
# BurningBungalow images (hothouse by default). Not part of `make test`.
firmware-check:
	bun tools/firmware_check.ts

clean:
	rm -rf build daisy/build

.PHONY: host render test midi-doc check-midi-doc morph-check firmware-check clean
