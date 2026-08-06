# Experiment 024: post-update query 026

Status: executed safely on 2026-08-05 after Experiment 023 and a second
RTC-backed cold power cycle. The command was consumed, no response descriptor
was consumed, and the response canary was unchanged.

## Question

Does the completed official OCTO firmware update, by itself, make the resident
query service available to the bounded Linux transport after a true cold boot?

## Preconditions

- PCI identity `1a00:0002`, subsystem `1a00:0005`.
- Endpoint alone in IOMMU group 16 and unbound.
- PCI command word zero before the read-only baseline capture.
- DMA control `0x0001fe00`.
- All 176 ordinary ring-register words zero.
- All eight DSP-ready values stable and bit zero set.

An initial attempt before the cold cycle refused because an orderly Windows
shutdown had left ring base registers programmed. It made no startup, ring,
DMA, interrupt, or command write. The cold cycle restored the exact guarded
baseline rather than weakening the precondition.

## Result

The existing Experiment 015 connect sequence was reused unchanged. It maps 64
ring pages and one canary page, contains DMA in the isolated Type 1 IOMMU, and
restores the complete transport before VFIO reset.

| Observation | Result |
|---|---|
| Eight connect commands and DSP0 clock command | Consumed in 1 ms |
| Query 026 `0x00260001` | Consumed |
| Response hardware read index | Did not advance |
| Response payload | Five unchanged `0xa5a5a5a5` words |
| Expected header `0x800c0005` | Absent |
| DMA writes outside response prefix | None |
| All eight DSPs ready after timeout | Yes |
| Explicit transport restoration | Passed |
| VFIO reset recovery | Passed |

The raw console result has SHA-256
`78c54edcfe8c47e4abd70258320bb1eb6b002b165a025335b4611f3dbc71b5b3`.
The same fields, with the experiment number corrected for publication, are in
[`experiment-024-result.json`](experiment-024-result.json).

## Interpretation

The persistent firmware update is not sufficient to expose the authorization
query service after a cold boot. The official Windows runtime must perform a
volatile framework, session, or program-level action that this probe does not
reproduce. Another speculative resident query is therefore low value.
Experiment 025 followed the official plug-in path and captured the first
nonzero responses plus two resource successes, although the complete plug-in
load still ended in error `-38`.
