# ducky: the Ducky NAM player pedal on the Daisy Seed in a Cleveland Music Co. HotHouse (src/board.h). Each APP builds
# one image, build/<BOARD>-<APP>/ducky_<APP>.bin (build/hothouse-xfade/ducky_xfade.bin by default):
# - xfade (the default): the pedal, the xfade build: the morph crossfades two models (src/ducky.cpp, DK_MORPH_XFADE=1);
# - weight: the pedal, the weight build: the morph blends one model's weights (src/ducky.cpp, DK_MORPH_XFADE=0);
# - bench: the A2-Lite, IR, EQ and compressors' benchmark and device check, no audio (src/bench.cpp);
# - latency: the FX loop's round trip and the channels' crosstalk (src/latency.cpp; docs/LOOP.md).
# Presets (models and IRs) live in QSPI, written as a bank by tools/preset_bank.ts and tools/flash_when_ready.sh, or
# slot by slot over USB-MIDI by tools/pedal.ts and the editor (docs/FIRMWARE.md "Preset store").
# From libDaisy's core Makefile: `make program-boot` installs the Daisy bootloader through the STM32's ROM DFU, and
# `make program-dfu` flashes $(BUILD_DIR)/$(TARGET).bin to 0x90040000 through the bootloader's DFU. A new Seed and
# the whole procedure: docs/FIRMWARE.md "Getting started".

# The app runs from SRAM, loaded from QSPI by the Daisy bootloader.
export APP_TYPE = BOOT_SRAM

# BOARD: hothouse (default, Cleveland Music Co. HotHouse). Other hosts are added here and in src/board.h.
BOARD ?= hothouse
ifeq ($(BOARD),hothouse)
BOARD_DEF = -DDUCKY_BOARD_HOTHOUSE=1
else
$(error BOARD must be hothouse, not '$(BOARD)')
endif

# Source lists must be set before including the core Makefile: it evaluates vpath directives at parse time.
# One build directory per board and app (the objects differ); libDaisy's mkdir has no -p.
# Each image has its own target name too, so the two pedal images (xfade, weight) ship side by side in a release.
APP ?= xfade
ifeq ($(filter $(APP),xfade weight bench latency),)
$(error APP must be xfade, weight, bench or latency, not '$(APP)')
endif
TARGET = ducky_$(APP)
# The app's main file; xfade and weight are both src/ducky.cpp (DK_MORPH_XFADE, below).
MAIN_SRC = src/ducky.cpp
ifeq ($(APP),bench)
MAIN_SRC = src/bench.cpp
else ifeq ($(APP),latency)
MAIN_SRC = src/latency.cpp
endif
CPP_SOURCES = $(MAIN_SRC) src/usb_link.cpp src/memory.cpp
override BUILD_DIR = build/$(BOARD)-$(APP)
$(shell mkdir -p $(BUILD_DIR))

# libDaisy is the daisy/libDaisy submodule (v9.0.0).
LIBDAISY_DIR ?= libDaisy
# Stop with the fix if the libDaisy submodule, its nested driver submodules, or its library is missing.
ifeq ($(wildcard $(LIBDAISY_DIR)/core/Makefile),)
$(error libDaisy not found at $(LIBDAISY_DIR). From the repo root run: git submodule update --init --recursive && make -C daisy/libDaisy)
endif
ifeq ($(wildcard $(LIBDAISY_DIR)/Drivers/STM32H7xx_HAL_Driver/Inc/stm32h7xx_hal.h),)
$(error libDaisy's driver submodules are missing under $(LIBDAISY_DIR). From the repo root run: git submodule update --init --recursive && make -C daisy/libDaisy)
endif
ifeq ($(wildcard $(LIBDAISY_DIR)/build/libdaisy.a),)
$(error libdaisy.a not built. From the repo root run: make -C daisy/libDaisy)
endif
include cmsis_fft.mk

# No instruction scheduling: GCC's Cortex-M7 model interleaves FP loads with multiply-accumulates, and on this core a
# load, store, move or add right after a multiply-accumulate waits about 3 cycles (docs/M7_PERFORMANCE.md).
# Unscheduled, the DSP's statement order is its instruction order (src/dsp/a2lite.h).
OPT = -O3 -fno-schedule-insns -fno-schedule-insns2
CPP_STANDARD = -std=gnu++17
# libDaisy's BOOT_SRAM script with .data/.bss in AXI SRAM, plus .dtcm_data/.itcm_text (copied at boot, src/memory.cpp),
# .dtcm_bss (zeroed at boot), .sdram_bss (the parsed presets) and .d2_bss (the bench's D2 SRAM comparison convolver;
# empty in the pedal).
LDSCRIPT = STM32H750IB_sram_ducky.lds

# The build id the Identity Reply carries (src/link.h): the repo's short commit, with +dirty when tracked
# files differ, or unknown outside git. $(BUILD_DIR)/build_id.c is rewritten only when the id changes, so an
# unchanged tree relinks nothing.
BUILD_ID_HEAD := $(shell git -C $(CURDIR)/.. rev-parse --short HEAD 2>/dev/null)
BUILD_ID := $(if $(BUILD_ID_HEAD),$(BUILD_ID_HEAD)$(if $(shell git -C $(CURDIR)/.. status --porcelain --untracked-files=no 2>/dev/null),+dirty),unknown)
BUILD_ID_C := $(BUILD_DIR)/build_id.c
BUILD_ID_SRC := const char kBuildId[] = "$(BUILD_ID)";
ifneq ($(BUILD_ID_SRC),$(shell cat $(BUILD_ID_C) 2>/dev/null))
$(shell printf '%s\n' '$(BUILD_ID_SRC)' > $(BUILD_ID_C))
endif
C_SOURCES = $(BUILD_ID_C) $(addprefix $(LIBDAISY_DIR)/Drivers/CMSIS-DSP/Source/,$(CMSIS_FFT_FILES))

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
.PHONY: all clean program program-dfu program-boot openocd debug debug_client

C_DEFS += $(BOARD_DEF)
ifeq ($(APP),weight)
# The weight build: one model runs, its weights blended into the target's (src/ducky.cpp; the xfade build runs both
# models and crossfades their outputs).
C_DEFS += -DDK_MORPH_XFADE=0
endif
C_DEFS += $(CMSIS_FFT_DEFS)
C_INCLUDES += -I$(LIBDAISY_DIR)/Drivers/CMSIS-DSP/PrivateInclude
CMSIS_OBJS := $(addprefix $(BUILD_DIR)/,$(notdir $(CMSIS_FFT_FILES:.c=.o)))
# Recompile CMSIS when its table selection changes: the core rule's prerequisites do not include cmsis_fft.mk.
$(CMSIS_OBJS): cmsis_fft.mk
# CMSIS-DSP's FFT with GCC's scheduling on: it is not written as islands, and unscheduled its 256-point real FFT cost
# 13,070 cycles against 9,330 scheduled (the 64-point one 2,990 against 2,080; docs/M7_PERFORMANCE.md). Same results.
$(CMSIS_OBJS): OPT = -O3

ifeq ($(APP),bench)
# src/bench.cpp .incbins these fixtures by absolute path; rebuild when they change.
C_DEFS += -DDK_REPO_ROOT='"$(abspath $(CURDIR)/..)"'
$(BUILD_DIR)/bench.o: ../tests/fixtures/nam/A2.bin ../tests/fixtures/nam/input.wav ../tests/fixtures/nam/A2.input.ref.wav \
  ../tests/fixtures/ir/decay.bin
endif

# Relink when the linker script changes: the core link rule lists only $(OBJECTS) and Makefile as prerequisites.
$(BUILD_DIR)/$(TARGET).elf: $(LDSCRIPT)
# src/usb_link.cpp flushes EP0's stale IN packet on each SETUP: libDaisy's calls to USBD_LL_SetupStage go through it.
LDFLAGS += -Wl,--wrap=USBD_LL_SetupStage
