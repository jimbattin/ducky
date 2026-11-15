// Memory placement (STM32H750IB_sram_ducky.lds).
//   DK_ITCM: a function in ITCM (zero-wait instruction fetch). An inlined copy runs from its caller's section, so every
//   caller of a DK_ITCM function is itself DK_ITCM or cold (Init: never per sample). Each use gets its own input
//   section (.itcm_text.N): GCC rejects a named section that holds both inline (COMDAT) functions, such as the DSP's
//   header functions, and ordinary ones, such as AudioCallback, in one translation unit. GCC 13 drops the section
//   attribute from template instantiations (function templates and class-template members): a DK_ITCM template runs
//   from ITCM only when inlined into its caller, so hot ones must be [[gnu::always_inline]]; `arm-none-eabi-nm` on the
//   image shows what landed where.
//   DK_DTCM: a variable in DTCM (zero-wait data, not cached). Any initialiser works.
//   DK_DTCM_BSS: a zero-initialised variable in DTCM (.dtcm_bss), NOLOAD, zeroed at boot by memory.cpp, so it has no
//   load image. Only for objects whose static initialiser is all zeros (no dynamic constructor).
//   DK_SDRAM: a variable in the external SDRAM (.sdram_bss). NOLOAD: startup neither initialises nor zeroes it, so
//   its owner clears it. No initialiser.
//   DK_D2: a variable in D2 SRAM (.d2_bss), NOLOAD, cleared by its owner.
// ITCM and DTCM are linked to run in the TCMs and loaded in SRAM; memory.cpp copies them from their SRAM load image
// in .preinit_array, after startup's .data/.bss init and before any C++ constructor or main(). Calls between ITCM
// (0x00000000) and SRAM (0x24000000) are out of BL range; GNU ld inserts long-branch veneers for them.
// On the host (tests) the macros are empty.
#pragma once

#if defined(__arm__)
#define DK_ITCM_STR(x) #x
#define DK_ITCM_SECTION(n) ".itcm_text." DK_ITCM_STR(n)
#define DK_ITCM __attribute__((section(DK_ITCM_SECTION(__COUNTER__))))
#define DK_DTCM __attribute__((section(".dtcm_data")))
#define DK_DTCM_BSS __attribute__((section(".dtcm_bss")))
#define DK_SDRAM __attribute__((section(".sdram_bss")))
#define DK_D2 __attribute__((section(".d2_bss")))
#else
#define DK_ITCM
#define DK_DTCM
#define DK_DTCM_BSS
#define DK_SDRAM
#define DK_D2
#endif
