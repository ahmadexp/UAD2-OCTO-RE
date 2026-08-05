# Runtime response state

The transport can dequeue commands, but the tested card has not written one
valid command response. This note separates a working DMA path from a missing
device runtime.

## Observed state progression

| State | Evidence | Response service |
|---|---|---|
| Cold endpoint | eight stable ready-poll values and cold-zero ring windows | not observed |
| Host transport started | all 16 rings published, eight DMA engines enabled, interrupt mapping reproduced | not observed |
| Resident commands dequeued | connect, query 026, and query 027 advance command read indices | no response write |
| HBUT transition attempted | exact extended header consumed, first payload descriptor not consumed | no response write |
| Runtime framework active | required by official system and plug-in services | not reached |
| Program active | requires framework, allocation, load, and completion ABI | not reached |

Command-ring consumption proves that the FPGA DMA front end recognizes the
ring and descriptor layout. It does not prove that a DSP runtime is executing
the submitted command or that a matching response service exists.

## Host system-information behavior

The updater reads a 168-byte GetSystemInfo record. Status `-0x5c` is handled
as a transient condition, converted to a retry result, and the version
comparison permits five reads at roughly 200 ms intervals. The client reaches
this operation through operation index `0x6f`.

The exact kernel implementation is now recovered. `UAD2System.sys` invokes a
PCIe device method that builds the record locally from cached object fields and
BAR MMIO. It returns `-0x5c` only while object field `+0x0c40` is clear. Driver
initialization and start set the field; stop clears it. Neither the base record
assembler nor the complete PCIe method submits a DSP ring command.

Consequently, this status supports a Windows driver lifecycle state, not a DSP
runtime state. Reproducing operation `0x6f` under Linux would duplicate BAR
reads already present in the passive tooling and would not produce the valid
DSP response required by the project.

The record is no longer wholly opaque. The updater's own report labels assign
driver, FPGA, DSP framework, DSP bootloader, serial, and auxiliary FPGA fields.
The recovered layout and exact field sources are documented in
[`system-information-record.md`](system-information-record.md).

## Why the full HBUT experiment did not resolve it

The exact PCIe driver confirms Experiment 021's extended header and
page-bounded descriptor chain. The experiment used a page-aligned IOVA, so an
official first payload descriptor would be 4 KiB. The observed stop at that
descriptor is therefore not explained by an incorrectly combined single DMA
object or a missing response descriptor.

Plausible remaining categories are deliberately unordered:

- a required device or DSP boot state before command `0x00120000`;
- a card-generated DMA fault not visible in the captured host registers;
- an accepted physical-address constraint beyond the recovered descriptor
  rules;
- persistent-update arbitration or reset sequencing;
- a firmware-aware hard-reset transition after the load flag is set;
- an inner-container validation path that stalls before reporting failure.

The adjacent commands `0x000d0000` and `0x000e0000` are not evidence that they
must bracket the initial update. The exact `UAD2System.sys` common dispatcher
selects one distinct target method for each of operations `0x67`, `0x68`, and
`0x69`; operation `0x69` does not automatically invoke the other two there.
The exact updater application also calls `LoadBinFile` directly from both its
single-update wrapper and its multi-device loop. `LoadBinFile` invokes only the
firmware-update vtable slot for `FBUT`, `GBUT`, and `HBUT`. The missing action
is therefore not a hidden automatic pre-operation or post-operation in either
recovered host caller. Submitting the neighboring commands speculatively would
add risk without a discriminating prediction.

The driver lifecycle does contain a different post-load branch. `LoadFirmware`
sets a host flag, and a later hard reset writes `0x0be0deaf` to DSP0 `+0x1a8`
instead of performing the ordinary BAR `+0x221c` reset pulse. This path is
confirmed independently in the exact Windows and symbolized macOS drivers. It
does not explain why Experiment 021 stopped at its first payload descriptor,
because no successful load response occurred. Issuing the magic after a
timeout would be non-discriminating and potentially persistent.

Framework properties 6, 7, and 8 are also eliminated as response candidates.
The symbolized PCIe driver implements them with local BAR and cached-object
reads. Property 6 is exactly the eleven-word pool map already captured by
Experiment 020. See [`framework-property-map.md`](framework-property-map.md).

## Next evidence, in order

1. Capture a lawful official update on disposable, recoverable hardware below
   the recovered host call boundary, including descriptor progress, device
   resets, the `+0x1a8` or `+0x221c` branch, return values, and timing. Do not
   capture or publish secrets.
2. Identify the first device-side consumer of the HBUT prefix or compatibility
   ID through static analysis.
3. Identify the DSP-side consumer of a form-zero `Bill` resource and decode
   one target-compatible inner core.
4. Only after volatility and recovery are demonstrated, define a new bounded
   hardware experiment with one changed variable and a unique expected result.

Repeating queries, changing descriptor size without evidence, or sending the
neighbor wrappers does not satisfy this standard.
