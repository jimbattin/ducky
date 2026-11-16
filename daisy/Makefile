# ducky: the Ducky NAM player pedal on the Daisy Seed in a Cleveland Music Co. HotHouse (src/board.h).
# APP=ducky (default): the pedal firmware, build/<BOARD>/ducky.bin. Presets (models and IRs) live in QSPI, written by
# tools/preset_bank.ts and tools/flash_when_ready.sh (docs/FIRMWARE.md "Preset store").
# APP=bench: the A2-Lite, IR and EQ benchmark and device check (src/bench.cpp, no audio),
# build/<BOARD>-bench/ducky_bench.bin.

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
APP ?= ducky
ifeq ($(APP),ducky)
TARGET = ducky
CPP_SOURCES = src/ducky.cpp src/usb_link.cpp src/memory.cpp
override BUILD_DIR = build/$(BOARD)
else ifeq ($(APP),bench)
TARGET = ducky_bench
CPP_SOURCES = src/bench.cpp src/usb_link.cpp src/memory.cpp
override BUILD_DIR = build/$(BOARD)-bench
else
$(error APP must be ducky or bench, not '$(APP)')
endif
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
# .dtcm_bss (zeroed at boot), .sdram_bss (the parsed presets) and .d2_bss (reserved for later milestones).
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

C_DEFS += $(BOARD_DEF)
C_DEFS += $(CMSIS_FFT_DEFS)
C_INCLUDES += -I$(LIBDAISY_DIR)/Drivers/CMSIS-DSP/PrivateInclude
# Recompile CMSIS when its table selection changes: the core rule's prerequisites do not include cmsis_fft.mk.
$(addprefix $(BUILD_DIR)/,$(notdir $(CMSIS_FFT_FILES:.c=.o))): cmsis_fft.mk

ifeq ($(APP),bench)
# src/bench.cpp .incbins these fixtures by absolute path; rebuild when they change.
C_DEFS += -DDK_REPO_ROOT='"$(abspath $(CURDIR)/..)"'
$(BUILD_DIR)/bench.o: ../tests/fixtures/nam/A2.bin ../tests/fixtures/nam/input.wav ../tests/fixtures/nam/A2.input.ref.wav \
  ../tests/fixtures/ir/decay.bin
endif

# Relink when the linker script changes: the core link rule lists only $(OBJECTS) and Makefile as prerequisites.
$(BUILD_DIR)/$(TARGET).elf: $(LDSCRIPT)
