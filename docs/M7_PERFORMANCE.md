# Cortex-M7 float performance

What the Daisy Seed's Cortex-M7 (STM32H750, 480 MHz, FPv5-D16) actually charges for float code, measured on the device, and how to write DSP that fits it. Read this before writing or optimising any kernel in `daisy/src/dsp/`.

These measurements took the A2-Lite engine from 570 to 352 permille of the block period with no change to its arithmetic, and later to 342 at half the block size (`docs/MEASUREMENTS.md` "Bench"). Everything here was measured with GCC 13.3.rel1, code in ITCM, data in DTCM unless a section says otherwise. The numbers are observations; the mechanism inside the core is not established.

## The short version

1. **Count switches, not instructions.** A multiply-accumulate (`vfma`) costs 1 cycle and a float load half a cycle. A load, store, register move or add placed right after a multiply-accumulate waits about 3 cycles. Alternating them costs 4 cycles a pair instead of 1.5.
2. **Write islands.** Put every load a run of multiply-accumulates needs in front of it, then the run and nothing else. Size the run so its accumulators, weights and sources fit the 32 single-precision registers.
3. **Make statement order the instruction order.** GCC's scheduler interleaves loads with arithmetic, so device builds turn it off (`-fno-schedule-insns -fno-schedule-insns2`). Pin the values GCC still moves with `dkdsp::FpHere`.
4. **No data-dependent branches.** A mispredict costs 6–7 cycles. Use `std::fmax` (`vmaxnm`), never compare-and-select.
5. **No short counted loops in the hot path.** The core predicts no loop exit, so a 3-trip loop mispredicts every time it runs. Flatten it into its outer loop.
6. **A result is ready 3 cycles after its instruction.** An operation right behind the one it depends on waits. Batch producers, then consumers.
7. **Integer work is free.** Integer adds, loads and stores pair with float operations at no cost. Put pointer arithmetic right after a run, where the next load would wait anyway.
8. **Check the image, then the device.** Read the instruction stream ("Checking an image"), then run the bench. Host tests cannot see any of this.
9. **A sum that returns to memory: loads first, then each add beside its store.** An add and a store pair in one cycle; `load, add, store` per element measured 2.1 cycles alone, and grouping the loads saved 1.2 to 1.5 cycles a sum in the engine.
10. **No libc copies in the hot path.** newlib-nano's `memcpy` and `memmove` are byte loops in SRAM, and GCC calls them for any copy it does not expand inline. Use `dkdsp::CopyFloats` ("Copies").
11. **Data outside the TCMs: on cache lines, and prefetched.** A stream larger than the 16 KB D-cache misses on every line; align it to 32 bytes and ask for the next lines one island ahead ("Cached data").

## What pairs and what stalls

| First | Then | Cost |
|---|---|---|
| `vfma` | `vfma`, `vmul` | none: 1 cycle each |
| `vfma` | integer `add`, `ldr`, `str` | none: the pair takes 1 cycle |
| `vadd`, `vmul` | `vldr`, `vstr` | none: the pair takes 1 cycle |
| `vfma` | `vldr`, `vstr`, `vmov.f32` | **+3 cycles** |
| `vfma` | `vadd` | **+4 cycles** |
| any float op | an op that reads its result | +2 cycles (3-cycle latency) |
| conditional branch on data | | 6–7 cycles when mispredicted |

`vmul` is neutral: it mixes with multiply-accumulates and with loads and stores. Only the multiply-accumulate has the penalty. `vmla` has `vfma`'s throughput and latency; its pairings were not measured separately.

## Measurements

Each kernel is a loop of straight-line assembly; the figure is cycles per loop iteration ("How to measure"). The empty loop costs 0.94, included in every figure.

### One kind of instruction

| Body | Cycles | Reading |
|---|---|---|
| 36 `vfma` into 12 registers in turn | 35.98 | 1 a cycle |
| 36 `vfma` into 3 registers in turn | 35.98 | latency ≤ 3 |
| 36 `vfma` into 1 register | 107.99 | latency 3 |
| 36 `vmla` into 12 registers | 35.98 | same as `vfma` |
| 36 `vmul` / 36 `vadd`, independent | 35.98 / 35.98 | 1 a cycle |
| 36 `vmul` / `vadd` / `vmaxnm`, chained through 1 register | 107.99 each | latency 3 |
| 36 `vmov.f32`, independent | 18.92 | 2 a cycle |
| 24 `vldr s` | 12.98 | 2 a cycle |
| 12 `vldr d` | 12.98 | 1 a cycle, 2 floats each |
| `vldmia` of 24 singles / 12 doubles | 12.94 / 12.94 | 2 floats a cycle |
| 4 `vldmia` of 6 singles / 8 of 3 singles | 12.9 / 16.98 | an odd count rounds up |
| 24 integer `ldr` | 12.91 | 2 a cycle |
| 12 `vstr s` | 11.98 | 1 a cycle |
| 6 `vstr d` / `vstmia` of 12 singles | 6.98 / 6.94 | 2 floats a cycle |

So a load costs the same in any form, but a store is twice as cheap as a double or in a `vstm`. GCC emits neither for floats; only assembly can use that.

### Two kinds alternated, 36 pairs

| Pair | Cycles | Per pair |
|---|---|---|
| `vfma`, integer `ldr` / `add` / `str` | 36.98 / 36.99 / 36.72 | 1 |
| `vadd`, `vldr` / `vmul`, `vldr` | 36.98 / 37.00 | 1 |
| `vadd`, `vstr` / `vmul`, `vstr` | 36.98 / 36.98 | 1 |
| `vfma`, `vmul` | 71.90 | 2 |
| `vfma`, `vldr` | 143.97 | **4** |
| `vfma`, `vldr`, `vldr` (36 triples) | 162.9 | 4.5: the second load is nearly free |
| `vfma`, `vstr` | 143.91 | **4** |
| `vfma`, `vmov.f32` | 143.99 | **4** |
| `vfma`, `vadd` | 179.96 | **5** |
| `vmul`, then a `vadd` that uses it | 144.00 | 4: the add waits for the product |

### Islands: 36 `vfma` and a block of something else

| Body | Cycles | Reading |
|---|---|---|
| 12 × (1 `vldr`, 3 `vfma`) | 72.99 | 3 cycles per one-load island |
| The same, each load 3 or 6 instructions before its first use | 72.95 / 73.00 | hoisting the load does not help: it is the switch, not the data |
| 6 `vldr d`, then 36 `vfma` | 44.95 | one island |
| `vldmia` of 12, then 36 `vfma` | 44.89 | one island |
| 2 × (`vldmia` of 6, 18 `vfma`) | 47.95 | two islands |
| 36 `vfma`, then 12 `vldr s` | 48.96 | single `vldr` islands cost more than `vldm` ones |
| 36 `vfma`, 8 integer adds, 12 `vldr` | 46.88 | integer work after the run is free and shortens the wait |
| 36 `vfma`, 12 `vldr`, 8 integer adds | 52.98 | after the loads it costs its own 4 cycles |
| 36 `vfma`, 6 `vadd`, 12 `vldr` | 50.94 | |
| 36 `vfma`, 12 `vldr`, 6 `vadd` | 52.97 | |
| 36 `vfma`, then 12 `vstr` of the results / one `vstmia` | 49.96 / 45.98 | |
| 36 `vfma`, 12 `vstr`, 12 `vldr` | 55.98 | the loads after the stores cost 6 |

An island costs its loads at two a cycle plus 2 to 6 cycles. `vldm` and `vldr d` islands sit at the low end (about 2 + n/2 for n floats). Islands of single `vldr` vary: 3 for 1 load, 9 for 9, 8 for 10, 12 for 12 or for 18.

### Leaky ReLU, 12 values

| Form | Cycles each |
|---|---|
| `vcmpe; vmrs; bge; vmul`, random signs | 9.6 |
| `vcmpe; vmrs; bge; vmul`, all positive | 6.1 |
| `vcmpe; vmrs; it mi; vmulmi` (no branch) | 7.1 |
| `vmul; vmaxnm`, each maximum right after its product | 4.0 |
| 12 `vmul`, then 12 `vmaxnm` | 2.0 |
| `vldr; vadd; vstr` on one element (for comparison) | 2.1 |

`max(z, s·z)` equals `z ≥ 0 ? z : s·z` for every `z` when `0 < s < 1`, including `−0`, so the cheap form changes no result bit. GCC compiles the compare-and-select form to the data-dependent branch.

### Shapes for a 3×3 tap over a tile of frames

Per tap and frame the engine does 9 multiply-adds. The prototypes below hold the tile's sums in registers; "per frame" divides by the tile size.

| Shape | Live registers | Cycles | Per frame |
|---|---|---|---|
| 4 frames, the scheduled order: 9 weight loads, then 12 × (source load, 3 `vfma`) | 22 | 77.97 | 19.3 |
| 4 frames: 9 weights by `vldmia`, then 2 × (`vldmia` of 6 sources, 18 `vfma`) | 27 | 52.99 | 13.0 |
| 4 frames: 9 weights and 6 sources by `vldr`, 18 `vfma`, 6 sources, 18 `vfma` | 27 | 54.99 | 13.5 |
| 3 frames: 9 weights and 9 sources by `vldr`, then 27 `vfma` | 27 | 40.00 | 13.0 |
| 6 frames, one input at a time: 3 × (3 weights and 6 sources, 18 `vfma`) | 27 | 81.99 | 13.5 |
| 4 frames with two weight banks, loads spread between the `vfma` (2 taps) | 32 | 195.99 | 24.4 |

Per frame is (cycles − 0.94) / frames. The first row reproduces what the engine ran before (940 cycles a tap a block, 78 per tile-tap). Four frames with all 12 sources loaded at once needs 12 + 9 + 12 = 33 registers, one too many.

Whole-layer prototypes for a kernel-6 layer (bias, 6 taps, mixin, leaky ReLU, head sum, residual):

| Shape | Cycles per tile | Per frame |
|---|---|---|
| 6-frame tile, taps one input at a time, residual one output at a time | 730.9 | 121.7 |
| 4-frame tile, 9 weights per tap, residual one frame at a time | 513.0 | 128.0 |

The engine uses the 6-frame shape. In the device it runs at 137 cycles per frame (6,570 per block); the 15 on top of the prototype are loop control, per-chunk setup and closing the rings.

## Case study: the A2-Lite engine

Same bench, same arithmetic, same device check (241e-9) throughout.

| Version | Avg cycles | Max permille |
|---|---|---|
| Tap-major: each tap swept over the 48-frame chunk, sums in memory | 272,008 | 570.2 |
| The same, tap loop unrolled to two frames | 297,215 | 622.1 |
| 4-frame tiles, sums in registers across the taps, epilogue weights re-read after every store | 281,001 | 589.0 |
| The same, epilogue weights in locals | 247,744 | 519.8 |
| 6-frame tiles as islands, `vmaxnm` leaky ReLU, residual written straight into the next ring, last residual dropped | 177,299 | 370.7 |
| The same, taps × inputs as one loop | 168,369 | 351.9 |

- **The first four are ordinary embedded optimisation**: fewer loop iterations, no stores in the inner loop, sums in registers, weights hoisted. They moved the result by 9 % in total, and two of them made it worse.
- **The wrong model fitted.** The 4-frame tile's inner loop ran about 70 instructions in 78 cycles, so "one instruction per cycle" looked right. It was a coincidence: 36 multiply-adds at 1 cycle plus twelve load islands at 3 to 8 cycles each make the same 78. An inference from instruction counts is not a measurement.
- **The hint that was there:** re-reading up to 15 weights per frame cost 33,000 cycles a block, about 2 cycles a load. Loads measured alone cost half a cycle.
- **The last two changed no instruction counts worth mentioning.** They changed the order.

Where the 168,369 cycles go: taps 108 k (1.6 cycles per multiply-add), the rest of each layer 55 k, head 4.8 k, input 1.4 k. The rest of a layer is about 50 cycles per frame. The residual is 24 of them (measured: layer 22 has none). By the cost model the head sum is 6, the leaky ReLU 6, the mixin 4 and the bias 2; the remainder is loop control and per-chunk work.

Later, at 24-sample blocks (240,000 cycles a block, the same per-frame work), more changes of order and none of arithmetic, each adding to the one above:

| Version | Avg cycles | Max permille |
|---|---|---|
| As above, 24-frame chunks, the rings outside the object (`A2Lite::Bind`) | 87,034 | 365.7 |
| The chunk's last copy (the head sums' history) by float loads and stores, not the `memcpy` GCC made of the loop | 86,492 | 363.1 |
| The residual's three outputs as straight-line runs, each with its four weights pinned first | 85,813 | 361.4 |
| The head sums as 6 loads, then 6 adds each beside its store | 83,332 | 351.1 |
| The residual's sums the same way: its 6 loads of x after the run, then add and store in pairs | 81,437 | 342.2 |
| The input's subnormal flush on the bits (integer), not a float compare | 81,302 | 341.8 |

- **The loop over outputs could be unrolled after all.** Unrolled before, GCC shuffled sums between registers inside the runs; with each run's weights pinned by `FpHere` it does not, and the exit mispredict per tile goes (680 cycles a block).
- **Grouping the loads of a sum that returns to memory** and leaving each add beside its store saved 1.5 cycles a sum on the head sums (2,481 cycles a block for 1,656 sums) and 1.2 on the residual (1,895 for 1,584); alone, `vldr; vadd; vstr` measured 2.1 an element. GCC emits the pairs from adds written before the stores: it forwards each single-use sum to its store.
- **The input stage** fell from 709 to 552 cycles: a compare-and-select per sample became `tst; it eq; andeq` on the sample's bits, in a loop of its own in front of the rechannel's products.

## Steering GCC

What GCC 13.3 did to this code, and what holds the shape in `daisy/src/dsp/a2lite.h`:

| GCC does | Effect | Counter |
|---|---|---|
| Schedules each source load beside its multiply-adds | a 3-cycle wait per load | `-fno-schedule-insns -fno-schedule-insns2` in `daisy/Makefile`; write loads, then arithmetic |
| Emits a single-use load immediately before its use | the load lands inside the run | `FpHere(load)` where it should be |
| Copies a shared value (a bias) into each accumulator just before that accumulator's first multiply-add | a `vmov` inside the run for every accumulator | `acc = FpHere(bias)` before the run |
| Shuffles sums between registers when several islands are unrolled into straight-line code | `vmov` inside the run | one island per loop iteration: `#pragma GCC unroll 1` on the loop over islands |
| Computes `base + 3 * index + offset` with two integer adds per load | three instructions per source load instead of one | keep a pointer variable and index it with constants |
| Compiles `z >= 0 ? z : s * z` to `vcmpe; vmrs; bge` | a data-dependent branch | `std::fmax(z, s * z)`, which becomes `vmaxnm` without any fast-math flag |
| Contracts `a += b * c` to `vfma` | wanted; this is why the device differs from the host by 241e-9 | write one product per statement, in the reference's order |
| Drops section attributes from template instantiations | code lands outside ITCM | `[[gnu::always_inline]]` into a `DK_ITCM` caller (`docs/FIRMWARE.md` "Memory") |

`FpHere(v)` is an empty `asm` statement that takes `v` in an FP register and "modifies" it, so the value must exist in a register of its own at that point. It costs nothing and is a no-op on the host.

The register shuffles and the address arithmetic were found by reading the disassembly and fixed before that build was ever timed, so their cost on the device is not measured.

**This shape is not guaranteed by the language.** A compiler upgrade can undo it without failing a test: host output stays bit-exact and only the cycle count moves. The `bench gate` line is the regression test. Run the bench after any toolchain or flag change.

## Sizing a tile

The FPU has 32 single-precision registers (`s16`–`s31` are callee-saved and cost one push per call). Everything live inside a run must fit: accumulators, weights, sources.

- **Fewer, larger islands are better**, but the gain flattens: 18 to 27 multiply-adds per island are all within 4 % of each other above.
- **Spilling is fatal.** A spill is a store and a load inside a run. Leave a few registers free; the 6-frame tile holds 27.
- **Count the epilogue too.** The 6-frame tile's residual does not fit with all 12 of its weights (18 activations + 12 + 6 sums), so it runs one output at a time (18 + 4 + 6).
- **Prefer a tile that divides the block**, so the device never runs the one-frame tail.

## Not taken, and not measured

Ideas with an estimate but no device run:

- **Assembly islands with `vldmia`/`vstmia`**: 13.0 instead of 13.5 cycles per frame-tap in the prototypes, and half-price stores.
- **3-frame tiles**: slightly cheaper taps, more epilogue overhead per frame; about level.
- **Separate `vmul` and `vadd` instead of `vfma`** where memory and arithmetic must alternate (an in-place `y[i] += a·x[i]`): neither is penalised beside loads and stores, but the result is not fused, so it rounds differently.

Taken since, and measured ("Case study"): the head sum and residual update as grouped loads and add-store pairs, and the residual's loop over outputs unrolled.

Not measured at all: double precision; `vdiv`, `vsqrt`, `vcvt`; moves between core and FP registers; `vmaxnm`, `vsub`, `vneg` or `vabs` directly after a multiply-accumulate (assumed to behave like `vadd`); data in SDRAM; libDaisy, which is built separately with scheduling on.

## Copies

newlib-nano's `memcpy`, `memmove` and `memset` are byte loops (`ldrb; strb; cmp; bne`), linked in AXI SRAM and reached from ITCM through a veneer. GCC calls them for a `std::memcpy` or `std::copy` whose size it does not expand inline (a variable size, or a large fixed one), and for a plain copy loop it recognises as one. Measured in the IR convolver: its copies (608 bytes in every block, 2,048 more in the block that builds the tail's window) cost 1,500 and 6,100 cycles more than the same copies as float loads and stores, about 2.4 cycles a byte.

- **`dkdsp::CopyFloats`** (`dsp/blocks.h`) copies 8 floats at a time, 8 loads then 8 stores, each load through `FpHere`, which also keeps GCC from turning the loop back into a `memcpy` call. It is correct for ranges that overlap with the destination below the source.
- **Finding them:** disassemble and list the calls to `__memcpy_veneer`, `__memmove_veneer` and `__memset_veneer` from addresses below `0x10000`. The pedal's callback had four (MISO's copy to R Out on every block, the EQ's and the compressor's dry copies while they fade, the parked block's fill) and the convolver eight; the fill is the one left.
- **The bench can differ from the pedal here:** with the bench's constant block size GCC made a `memcpy` of the engine's last loop, which the pedal's variable size kept as a loop: 540 cycles a block that only the bench paid.

## Cached data

Data in AXI SRAM goes through the 16 KB D-cache (4 ways, 32-byte lines). The IR convolver's tail reads 40 KB of spectra once per frame, so every line is a miss; its multiply-accumulate islands read one tile (32 bytes) of two spectra each.

| Tail partition (32 tiles) | Cycles |
|---|---|
| Pool at any alignment (a tile over two lines) | 1,980 |
| Pool 32-byte aligned (a tile is one line) | 1,570 |
| The same, each island prefetching the next one's two lines (`__builtin_prefetch`, `pld`) | 1,170 |

- **1,170 is what the arithmetic costs:** 37 cycles a tile for 16 loads and 16 multiply-adds. The prefetch hides the whole miss behind the island before it.
- **`pld` is integer work:** at the top of the island, after the previous run, it costs nothing measurable. The head's spectra are in DTCM and need none, so the kernel takes a flag.
- **Engine rings in AXI SRAM** (the xfade build, `APP=xfade`: both engines' three rings of dilation 239, 44 KB of each one's 84, without prefetch): the second engine and the mix cost 360 permille live against the bench's 339 for an engine in DTCM. With the second engine's whole 92 KB object in AXI SRAM they cost 421 against 363.

## CMSIS-DSP's FFT

The IR convolver's FFT is CMSIS-DSP's C code, not written as islands. Compiled with Ducky's flags (no scheduling) it was 40 % slower than with GCC's scheduling on, which `daisy/Makefile` now gives those files:

| Real FFT (`arm_rfft_fast_f32`) | Unscheduled | Scheduled (`-O3`) |
|---|---|---|
| 256-point, forward / inverse | 13,070 / 14,350 | 9,330 / 10,290 |
| 64-point, forward / inverse | 2,990 / 3,300 | 2,080 / 2,310 |

`-O2` measured the same as `-O3`. Its code in ITCM instead of AXI SRAM changed nothing on the bench and 1 permille live, so it stays in AXI SRAM. In the convolver each 256-point transform costs about 500 cycles more than alone.

Measured only as whole stages (`docs/FILTERS.md`, `docs/MEASUREMENTS.md` "Bench"): the first EQ's recursive filters (two 3-slope cut filters and a bell) cost 15,135 cycles a 48-sample block, about 315 cycles per sample, including two `powf` and a `tanf` per cut filter and a `powf` for the bell per block.

Expectations that follow from the measurements but were not tested:

- **Recursive filters** are bound by the 3-cycle latency of each chained operation, not by throughput. Interleave independent sections or channels to fill the gaps.
- **Any kernel that reads a stream, multiplies and accumulates** (FIR, correlation, mixing) has the A2-Lite problem and the same fix.

**Core state** during the measurements: `ACTLR` `00001000`, `CCR` `00070200` (caches and branch prediction on), `ITCMCR` `0000003b`, `DTCMCR` `00000043`, `FPCCR` `c0000038`, `FPSCR` with FZ set.

**The audio callback is not in that state.** `dkfw::EnableFlushToZero` sets FZ in the main loop's `FPSCR`, and the bench runs there. An interrupt handler's floating-point context is created with the FZ of `FPDSCR`, which is 0: read inside `AudioCallback`, `FPSCR` is `20000010`. So on the pedal nothing flushes subnormals but the code: `A2Lite::BeginChunk` flushes the engine's input itself, and must.

## Checking an image

The instruction stream of a function as runs of classes: `M` multiply-accumulate, `L` float load, `S` float store, `U` `vmul`, `V` `vmov`, `A` any other float operation, `i` everything else.

```sh
arm-none-eabi-objdump -d --no-show-raw-insn daisy/build/hothouse-bench/ducky_bench.elf |
  awk -F'\t' '/RunLayer.*>:$/ {on = 1; next} /^$/ {on = 0}
    on && NF > 1 { m = $2
      print (m ~ /^vf[mn]|^vml/ ? "M" : m ~ /^vld|^vpop/ ? "L" : m ~ /^vst|^vpush/ ? "S" :
             m ~ /^vmul/ ? "U" : m ~ /^vmov/ ? "V" : m ~ /^v/ ? "A" : "i") }' |
  uniq -c | awk '{printf "%s%s ", $2, $1} END {print ""}'
```

The 6-frame tile of `A2Lite::RunLayer` reads:

```
L3 V15           bias: 3 loads, 15 copies
i16 L9 M18       per tap and input: 9 loads, 18 multiply-adds (a loop)
i3 L10 M18       mixin: 10 loads, 18 multiply-adds
U3 A3  (×6)      leaky ReLU: 3 products, 3 maxima per frame
L6 A1 S1 (×6)    head sums, two frames at a time (×3): 6 loads, then 6 adds each with its store
L4 V5 M18        residual, per output (×3, straight-line): 4 loads, 5 copies, 18 multiply-adds
L6 A1 S1 (×6)      and its 6 sums into the next ring: 6 loads of x, then add and store in pairs
```

An `M` run broken by `L`, `S`, `V` or `A` is a regression, whatever the tests say.

## How to measure

The kernels were a throwaway image, not kept. To rebuild it:

- **Kernels** in a `.s` file listed in `ASM_SOURCES` of a `daisy/Makefile` app branch (libDaisy's core Makefile assembles `.s`). Section `.itcm_text.<name>`, so the linker script puts them in ITCM.

  ```asm
      .syntax unified
      .cpu cortex-m7
      .fpu fpv5-d16
      .thumb
      .section .itcm_text.ubk,"ax",%progbits
      .balign 8
      .global ub_tap_now
      .thumb_func
      .type ub_tap_now, %function
  ub_tap_now:                      @ void ub_tap_now(float* p, uint32_t iters)
      vpush   {d8-d15}
      vldmia  r0, {d0-d15}         @ every register starts as a normal number
      .balign 8
  1:  vldr    s21, [r0, #0]        @ the body under test, straight-line
      vfma.f32 s0, s12, s21
      @ ...
      subs.w  r1, r1, #1
      bne.w   1b
      vpop    {d8-d15}
      bx      lr
  ```

- **Harness**: `bench.cpp`'s startup and DWT setup, then per kernel:

  ```cpp
  [[gnu::noipa]] uint32_t timeKernel(void (*fn)(float*, uint32_t), float* p, uint32_t iters) {
      __disable_irq();
      const uint32_t t0 = DWT->CYCCNT;
      fn(p, iters);
      const uint32_t c = DWT->CYCCNT - t0;
      __enable_irq();
      return c;
  }
  // cycles per iteration = (timeKernel(k, buf, 300) - timeKernel(k, buf, 100)) / 200, best of 3
  ```

  The difference of two iteration counts removes the call, the pushes and the setup. Log it as an integer times 100 (the image has no float `printf`).
- **Data**: a `DK_DTCM float[256]` refilled before every run with small normal values (`0.001f * (i % 61 + 1)`), so nothing overflows or goes subnormal over 300 iterations. For branch tests, tables of a few thousand random-sign and all-positive values walked by a post-incremented pointer.
- **One question per kernel.** Vary one thing between two kernels: the order, the distance, the form of the load. Bodies of 24 to 100 instructions keep the 0.94-cycle loop overhead negligible.
- **Then prototype the real shape** as one kernel before touching the C++. The whole-layer prototype predicted 122 cycles per frame; the engine measured 137.
