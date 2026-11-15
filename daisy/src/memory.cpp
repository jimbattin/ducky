// Copies the .itcm_text and .dtcm_data sections (memory.h, STM32H750IB_sram_ducky.lds) from their SRAM load image into
// ITCM/DTCM. Registered in .preinit_array: libDaisy's Reset_Handler calls newlib's __libc_init_array after the
// .data/.bss init, and that runs .preinit_array before .init_array, so the TCM contents exist before any constructor
// or main().
#include <cstdint>

#include "memory.h"

extern "C" uint32_t _itcm_text_start[], _itcm_text_end[], _itcm_text_load[];
extern "C" uint32_t _dtcm_data_start[], _dtcm_data_end[], _dtcm_data_load[];

namespace {

void copyWords(uint32_t* dst, uint32_t* end, const uint32_t* src) {
    while (dst < end) *dst++ = *src++;
}

void InitTcm() {
    copyWords(_itcm_text_start, _itcm_text_end, _itcm_text_load);
    copyWords(_dtcm_data_start, _dtcm_data_end, _dtcm_data_load);
}

}  // namespace

__attribute__((section(".preinit_array"), used)) static void (*gInitTcm)() = InitTcm;
