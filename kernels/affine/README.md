# Minimal affine SHARC kernel

This directory defines the first user-authored computation target:

```text
y[i] = binary32(binary32(x[i] * scale) + bias), 0 <= i < frames <= 64
```

The kernel is deliberately smaller than a biquad or convolution engine. It has
no state, no device-register access, no allocator, no interrupt dependency, and
no library calls. A valid hardware result must match independent binary32 test
vectors, not merely round-trip a captured vendor workload.

## Two contracts, kept separate

`uad_affine_entry` is a processor-level SHARC function with a documented C
signature. The ADI compiler can emit it as a Flat-V6 dynamically loadable
module with explicit sections, relocations, and an exported symbol. This
resolves the clear, user-authored side of the experiment.

The UAD resident framework adapter is still unresolved. No file here claims
that a Flat-V6 module is itself an accepted `Bill` resource, that the framework
calls this five-argument signature, or that a guessed SRAM address is safe to
execute. The hardware experiment remains disabled until a lawful accepted
upload path and a captured or debug-observed call adapter are verified.

## Build

Install and activate Analog Devices CrossCore Embedded Studio 2.12.1 for the
legacy SHARC family. Then run:

```bash
CCES_ROOT='/path/to/CrossCore Embedded Studio 2.12.1' \
  tools/build_sharc_affine.sh output/affine
```

On macOS or Linux with the Windows toolchain under Wine, also set
`WINEPREFIX`; the script selects `wine` automatically for `.exe` tools. It
refuses an unactivated compiler and leaves the licensing decision to the user.

The build produces `affine.dxe`, `affine.dyn`, and metadata-only JSON reports.
The output directory is not intended for checked-in proprietary tooling or
captured UAD resources.

`canonical-vector.json` is the first matched-measurement oracle. Regenerate a
verbose form, including decimal values and word indices, with:

```bash
python3 tools/affine_reference.py
```

Its non-identity scale, bias, and one-third sample distinguish real affine
execution from the recovered dry RealVerb roundtrip. Both guards must remain
unchanged.

## Hardware acceptance gate

Do not place this module into DSP memory until all of these checks pass:

1. the DXE is SHARC ELF32 for ADSP-21469 and exports
   `_uad_affine_entry`;
2. `elf2dyn` emits a valid revision-6 SHARC module with only known relocation
   forms;
3. the target code and data regions are observed allocations, not guessed
   framework addresses;
4. a data-only write/readback/restore probe proves the exact upload command;
5. the UAD entry adapter is recovered from a framework trace or JTAG snapshot;
6. DSP0 can be stopped and reset independently before the first call; and
7. input, output, guard words, timeout, and recovery are all bounded.
