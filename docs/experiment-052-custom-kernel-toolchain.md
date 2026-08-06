# Experiment 052: user-authored SHARC kernel toolchain

## Objective

Turn the arbitrary-code blocker into a reproducible sequence with one narrow
target: a 64-sample affine transform on DSP0. The experiment separates three
layers that earlier work had conflated:

1. valid ADSP-21469 machine code and relocation metadata;
2. a UAD-accepted upload object; and
3. the UAD framework entry and buffer adapter.

The first layer is now implemented. The second and third remain hardware
research gates. No authenticated vendor object is modified, no signing secret
is extracted, and no compiler license check is bypassed.

## Official processor-format reference

Analog Devices still distributes CrossCore Embedded Studio 2.12.1 for the
legacy SHARC family. The installed compiler identifies itself as SHARC
8.16.1.0 and includes `cc21k`, `easm21k`, `elfloader`, `elf2dyn`, ADSP-21469
processor definitions, runtime objects, and no-hardware-required ADSP-21469
dynamically-loadable-module examples.

The installed compiler requires a valid Analog Devices license. Its 90-day
evaluation is a lawful path, but activation requires user-supplied registration
details. The isolated installation is therefore left unactivated. The build
script fails closed at the vendor license check.

The distribution also includes prebuilt ADSP-21469 images that can be inspected
without compiling. The hash-locked `469_spi.dxe` is ELF32 little-endian for
machine `0x85` (SHARC), processor ADSP-21469, silicon revision 0.2. It has one
load segment at `0x0008c000`, 1,296 file bytes, and no remaining relocations.

Running the official loader with `-NoKernel` gives a standard big-endian block
stream:

| Block | Count | Target | Payload |
|---|---:|---:|---:|
| `ZERO_L48` | 5 | `0x0008c000` | 0 bytes |
| `INIT_L48` | 211 | `0x0008c005` | 1,266 bytes |
| `FINAL_INIT` | 0 | `0x00000000` | 0 bytes |

This verifies that one normal-word SHARC instruction occupies six file bytes
and one PM48 address unit. It also gives a clear negative control for the
previous scan that found no such block stream in transmitted RealVerb bodies.

## ADI Flat-V6 dynamic executable

The ADSP-21469 examples use a more relevant format than a reset loader. Their
linker produces an ELF `DYNAMIC` image with `.text`, `.data`, and zero-filled
`.bss` sections. `elf2dyn` converts that image into the format consumed by
`libdyn`.

The installed headers and loader source define that format exactly:

| Structure | Size | Relevant fields |
|---|---:|---|
| header | 56 bytes | `bFLT`, revision 6, family 2 (SHARC), section-table offset/count, relocation offset/count, build date, six zero words |
| section | 28 bytes | name file offset, content file offset, byte count, alignment, byte width, flags, mapped address |
| relocation | 12 bytes | reference offset, partial result, reference section, symbol section, dynamic relocation type |

The section byte width distinguishes 16-bit, 32-bit, 40-bit, 48-bit, and other
target address spaces. `.expstr` and `.expsym` carry exported names and their
relocated addresses. The loader validates the image, allocates each section in
a matching-width heap, copies contents, applies relocations, looks up an
exported function, and calls it.

For SHARC, the converter maps the supported ELF relocation types in the
`0x0b..0x19` family into dynamic types `0x01` through `0x3a`. The installed
header explicitly marks `0x17` unsupported and defines no dynamic form for
`0x15`, `0x16`, or `0x18`. The dynamic type records both source and destination
space widths as well as the instruction/data field being changed. The
recovered operations cover:

- 24-bit and 32-bit absolute addresses in 48-bit instructions;
- 32-bit pointers in data;
- short and long PC-relative branches;
- 6-bit and 16-bit data-address fields;
- VISA address fields; and
- 16-bit pointer locations.

This resolves the generic ADSP-21469 executable, exported-entry, and relocation
formats. It does not prove that a decoded UAD `Bill` core is a Flat-V6 module.
The transmitted cores remain high entropy and expose neither `bFLT` nor the
clear section table. Flat-V6 is now a concrete post-decode hypothesis that can
be tested against a lawful memory trace.

## Implemented affine artifact

[`../kernels/affine/affine.c`](../kernels/affine/affine.c) exports
`_uad_affine_entry` with this processor-level contract:

```c
int uad_affine_entry(const float *input, float *output,
                     unsigned int frames, float scale, float bias);
```

It accepts at most 64 DM32 binary32 samples, rejects null pointers and larger
counts, writes exactly `frames` output words, and has no MMIO, flash, FPGA,
interrupt, allocator, or library dependency. The build disables floating-point
reassociation and contraction so matched measurements can use separately
rounded binary32 multiplication and addition.

[`../tools/affine_reference.py`](../tools/affine_reference.py) produces the
canonical matched vector in
[`../kernels/affine/canonical-vector.json`](../kernels/affine/canonical-vector.json).
It uses scale `0.625`, bias `-0.09375`, signed values, zero, fractions, and a
binary32 one-third input. The expected words are deliberately not an identity
mapping, so the recovered dry RealVerb roundtrip cannot satisfy this test.

[`../tools/build_sharc_affine.sh`](../tools/build_sharc_affine.sh) drives the
official compiler and `elf2dyn`. It then uses
[`../tools/inspect_sharc_image.py`](../tools/inspect_sharc_image.py) to reject
the wrong processor, malformed sections, a missing defined global DXE entry,
undefined symbols, a missing named DLM export, excessive code, invalid
references, or unknown dynamic relocation types. No built binary is claimed
until the user activates CCES and the validator passes.

## Two code-shaped RealVerb allocations

The official ADSP-21469 memory map aliases block-0 DM32 and PM48 storage. A
DM32 bit offset maps to the same physical SRAM bit offset in PM48. Therefore:

```text
pm48 = 0x8c000 + ((dm32 - 0x92000) * 32) / 48
```

Two adjacent RealVerb private allocations are exactly aligned in both spaces:

| Private index | DM32 span | PM48 alias | Capacity |
|---:|---|---|---:|
| 2 | `0x9cede`, 150 dwords | `0x93494..0x934f7` | 100 instructions |
| 1 | `0x9cf74`, 150 dwords | `0x934f8..0x9355b` | 100 instructions |

[`../tools/sharc_memory_alias.py`](../tools/sharc_memory_alias.py) reproduces
the conversion and refuses an out-of-range or partial span.

These are now the highest-priority post-decode trace targets. Their exact
PM48 alignment makes code plausible, but not proven. They were allocated as
private zero-filled resources, and a 150-dword state buffer can have the same
shape by coincidence. They must be read through a lawful debug interface or
observed during resource loading before any instruction write is attempted.

## Accepted-object and entry adapter plan

The shortest defensible path no longer begins by guessing a signed `Bill`.
It is:

1. activate the official CCES evaluation and build the hash-locked affine DXE
   and Flat-V6 module;
2. cold-activate the unmodified UAD framework and replay the already proven
   exact RealVerb allocation;
3. prove `0x00080004` plus a following DMA descriptor as a data write using a
   single zero-valued padding word in the known private Process object;
4. read that word before, after marker write, and after restoration through the
   proven Process-coupled readback, with reset on any mismatch;
5. attach an official ADI emulator to the likely split four-DSP JTAG headers
   `H1` and `H4`, after continuity confirms ground, power, TCK, TMS, TDI, TDO,
   reset, and chain order;
6. stop DSP0 at the resource consumer, trace one known RealVerb object from
   public pool input to private memory, and test for a relocated `bFLT` image;
7. capture the actual function-call register/stack state at the first private
   Process dispatch;
8. implement a UAD adapter stub that translates that observed state to
   `_uad_affine_entry`; and
9. load only into an observed code allocation, run one 64-sample vector with
   guard words and a deadline, restore the official image, and reset DSP0.

Step 3 is data-only. Step 6 observes an official object. The first custom
instruction write occurs only after the address, relocation result, and call
state are all known.

## Decisive pass criteria

The user-authored result is complete only when all of the following are saved:

- source and compiler options;
- DXE and DLM hashes plus metadata-only section/relocation reports;
- pre-write, post-write, and post-restore data-readback hashes;
- observed code allocation and entry address;
- input vector, scale, bias, expected binary32 output, and guard words;
- request-correlated DSP0 completion and bounded output;
- non-target ring and memory canaries for DSP1 through DSP7; and
- explicit stop, per-DSP reset, whole-device recovery, and unchanged official
  workload retry.

Until those criteria pass, the repository has a complete user-authored SHARC
build artifact and a narrowed debug plan, but not a user-authored UAD kernel.

Machine-readable provenance and claims are in
[`data/experiment-052-custom-kernel-toolchain/result.json`](data/experiment-052-custom-kernel-toolchain/result.json).
