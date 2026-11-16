# CMSIS-DSP sources and table selection for dkdsp::IrConvolver's 64-point real FFT (src/dsp/convolver.h).
# libdaisy.a does not contain CMSIS-DSP: the firmware (daisy/Makefile) and the host build (Makefile) compile these.
# The 64-point real FFT runs a 32-point complex one, which arm_cfft_init_f32 takes from arm_const_structs.c's
# arm_cfft_sR_f32_len32 (the float bit-reversal table, BITREVIDX_FLT); a missing table makes Init return false.
CMSIS_FFT_FILES := TransformFunctions/arm_rfft_fast_f32.c TransformFunctions/arm_rfft_fast_init_f32.c \
  TransformFunctions/arm_cfft_f32.c TransformFunctions/arm_cfft_init_f32.c TransformFunctions/arm_cfft_radix8_f32.c \
  TransformFunctions/arm_bitreversal2.c CommonTables/arm_common_tables.c CommonTables/arm_const_structs.c
CMSIS_FFT_DEFS := -DARM_DSP_CONFIG_TABLES -DARM_FFT_ALLOW_TABLES -DARM_TABLE_TWIDDLECOEF_F32_32 \
  -DARM_TABLE_BITREVIDX_FLT_32 -DARM_TABLE_TWIDDLECOEF_RFFT_F32_64
