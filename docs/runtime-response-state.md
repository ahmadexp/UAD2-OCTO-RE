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

## Official not-ready behavior

The updater reads a 168-byte GetSystemInfo record. Status `-0x5c` is handled
as a transient condition, converted to a retry result, and the version
comparison permits five reads at roughly 200 ms intervals. The client reaches
this operation through operation index `0x6f`.

That behavior supports a state machine with an explicit not-ready phase. It
does not prove that `-0x5c` is the card's current result under Linux because
the Linux transport has not reproduced the Windows control interface above
the ring layer.

The record is no longer wholly opaque. The updater's own report labels assign
driver, FPGA, DSP framework, DSP bootloader, serial, and auxiliary FPGA fields.
The recovered layout is documented in
[`system-information-record.md`](system-information-record.md). A captured
operation-`0x6f` response is still required to connect those host fields to the
card's current state.

## Why the full HBUT experiment did not resolve it

The exact PCIe driver confirms Experiment 021's extended header and
page-bounded descriptor chain. The experiment used a page-aligned IOVA, so an
official first payload descriptor would be 4 KiB. The observed stop at that
descriptor is therefore not explained by an incorrectly combined single DMA
object or a missing response descriptor.

Plausible remaining categories are deliberately unordered:

- a required device or DSP boot state before command `0x00120000`;
- an updater-side operation not yet connected to the ring transition;
- a card-generated DMA fault not visible in the captured host registers;
- an accepted physical-address constraint beyond the recovered descriptor
  rules;
- persistent-update arbitration or reset sequencing;
- an inner-container validation path that stalls before reporting failure.

The adjacent commands `0x000d0000` and `0x000e0000` are not evidence that they
must bracket the initial update. The exact `UAD2System.sys` common dispatcher
selects one distinct target method for each of operations `0x67`, `0x68`, and
`0x69`; operation `0x69` does not automatically invoke the other two there.
Submitting the neighboring commands speculatively would add risk without a
discriminating prediction.

## Next evidence, in order

1. Capture operation `0x6f` through the lawful official stack and correlate
   the recovered 168-byte fields with the OCTO's live state.
2. Capture a lawful official update on disposable, recoverable hardware at the
   driver boundary, including operation order, return values, resets, and
   timing. Do not capture or publish secrets.
3. Identify the first device-side consumer of the HBUT prefix or compatibility
   ID through static analysis.
4. Identify the DSP-side consumer of a form-zero `Bill` resource and decode
   one target-compatible inner core.
5. Only after volatility and recovery are demonstrated, define a new bounded
   hardware experiment with one changed variable and a unique expected result.

Repeating queries, changing descriptor size without evidence, or sending the
neighbor wrappers does not satisfy this standard.
