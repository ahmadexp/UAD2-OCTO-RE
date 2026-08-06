# Runtime response state

The tested card now writes valid command responses through the official
runtime. This note separates those partial runtime services from a complete
program load.

## Observed state progression

| State | Evidence | Response service |
|---|---|---|
| Cold endpoint | eight stable ready-poll values and cold-zero ring windows | not observed |
| Host transport started | all 16 rings published, eight DMA engines enabled, interrupt mapping reproduced | not observed |
| Resident commands dequeued | connect, query 026, and query 027 advance command read indices | no response write |
| Bounded HBUT attempt | exact extended header consumed, first payload descriptor not consumed | no response write |
| Official HBUT update | extended header, all 625 payload descriptors, and response descriptor consumed | completion page stayed zero |
| Official signed runtime | all 16 rings published; two DSP0 command pairs and response descriptors consumed | exact-boundary response targets stayed zero |
| Post-update cold query | connect and query 026 consumed after RTC cold boot | response descriptor and canary untouched |
| Official plug-in host active | four repeatable `0x80030302` responses | stable 768-entry four-state table plus request token |
| Official resource loader | form-zero resources `0x120` and `0xd0` submitted at observed pool offsets | exact `0x80070004` intermediate success for both |
| RealVerb-Pro instantiated | VST3 user interface opened through the official host | disabled with `-38` after an all-zero response |
| Linux adopted framework | fresh Linux IOMMU rings published after the VM exited in DMA reset | query 026 returned `0x800c0005` in 1 ms |
| Exact first RealVerb object | private hash-locked `0x12b` target submitted from Linux | exact `0x80070004` success in 1 ms |
| Eight-engine loader isolation | exact `0x12b` object targeted independently at DSP0 through DSP7 | all eight accepted; every non-target index set stayed unchanged |
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

## What the official HBUT trace resolved

Experiment 023 traced the exact official updater through the same PCIe driver
and physical card. The command hardware read index advanced from 4 to 630,
covering the extended header, 624 full-page descriptors, and the 2,192-byte
tail descriptor. The response hardware read index advanced from 2 to 3. The
large-block descriptor shape is therefore live-validated, not merely recovered
statically.

The response completion page remained zero. During the post-update signed
runtime, QEMU was paused at the exact response-read-index boundary and the
second target page remained all zero at the boundary, 5 ms later, and 100 ms
later. Response index advancement must therefore be described as descriptor
consumption, not a successful reply. Experiment 024 further showed that the
persistent update alone does not expose query 026 after a cold boot.

Experiment 025 then activated the missing official plug-in path. Four response
targets contained header `0x80030302`, a command-correlated token, and the same
768-dword body. Static recovery now maps those wire values exactly to Demo not
started, Authorized, Authorized, and Auth update required. The two authorized
wire encodings deliberately collapse to one host state.

The same run produced exact `0x80070004` intermediate successes for official
form-zero `Bill` resources `0x120` and `0xd0`. Their response IDs and command
words match their submitted envelopes. RealVerb-Pro later displayed the
official device-not-responding error `-38`, which the recovered resource parser
assigns to an all-zero four-dword response.

Experiment 026 identified the first zero response target as resource `0x12b`.
Experiment 028 then submitted the exact same object from Linux and received
success in 1 ms. The result disproves intrinsic object rejection as the meaning
of the earlier zero. The invasive boundary capture and Linux replay used
different observation and framework-state contexts, so their exact causal
difference remains unassigned.

Experiment 029 changed one bit at the first and last preserved-prefix bytes and
at four trailing-core positions. All six changes were rejected with structured
`0xf001` status while unchanged controls succeeded. This proves device-side
integrity or authentication over both body regions without identifying the
algorithm. Experiment 030 repeated exact acceptance on every DSP with no
non-target ring-index changes.

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

The driver lifecycle contains a post-load branch in which `LoadFirmware` sets a
host flag and a later hard reset can write `0x0be0deaf` to DSP0 `+0x1a8`
instead of pulsing BAR `+0x221c`. The official update trace did not contain that
magic write, and the post-update orderly shutdown used the ordinary hard-reset
pulse. It should not be synthesized independently because its device-side
meaning remains unknown.

Framework properties 6, 7, and 8 are also eliminated as response candidates.
The symbolized PCIe driver implements them with local BAR and cached-object
reads. Property 6 is exactly the eleven-word pool map already captured by
Experiment 020. See [`framework-property-map.md`](framework-property-map.md).

## Next evidence, in order

1. Identify the DSP-side consumer of the accepted form-zero `0x12b` object and
   capture its decoded cleartext without publishing proprietary bytes.
2. Recover the segment, relocation, entry-point, and runtime-reservation
   records from that cleartext.
3. Define a bounded DSP0 heartbeat with a unique response, output, timeout, and
   recovery oracle.
4. Only after that run should buffer, program, submit, and wait UAPI operations
   be enabled.

Repeating queries, changing descriptor size without evidence, or sending the
neighbor wrappers does not satisfy this standard.
