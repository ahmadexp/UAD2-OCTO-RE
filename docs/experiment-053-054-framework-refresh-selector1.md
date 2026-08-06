# Experiments 053 and 054: framework refresh and private selector 1

## Result

An RTC power-off cycle followed by the official Windows driver republished all
eight command rings, all eight response rings, and all eight DMA-enable paths.
The guest initially stopped at the Windows boot handoff. That state and its
trace were preserved, then one virtual-machine reset completed the boot. The
official guest was shut down cleanly, leaving the endpoint unbound for one
bounded Linux trial.

The refreshed Linux trial accepted all 13 captured RealVerb resources,
consumed all 33 zero-allocation commands and the exact memory specification,
completed one two-channel 64-sample DSP0 Process job, and returned a valid
private-selector response for allocation index 1.

Selector 1 resolves to dword address `0x0009cf74` and length 150 dwords. The
response header was:

```text
80010098 01000000
```

All 150 payload dwords were zero. Their 600-byte little-endian SHA-256 is
`bd50e12c55dda3ee443c1cb6d71c7bcf6351c4ec96f7bc8d6adec015d1192eea`.
This rules out the selected private allocation as a clear executable image
under the recovered Process-coupled readback path. It does not prove that the
decoded Bill code is absent from every DSP memory region or that the readback
operation can address program memory generally.

The sanitized machine-readable result is
[`data/experiment-053-054-framework-refresh-selector1/result.json`](data/experiment-053-054-framework-refresh-selector1/result.json).
It contains hashes and derived metadata, but no proprietary resource body.

## Experiment 053: official framework refresh

The Linux host passed these preconditions before the RTC cycle:

- the reference guest and its software TPM were stopped;
- the UAD endpoint was unbound;
- PCI command was `0x0002`; and
- the RTC wake source was available.

The host then executed a 45-second RTC power-off. After restart it had boot ID
`b7890c34-139c-4df7-9dba-38e7588793bd`, the endpoint remained unbound, and PCI
command was `0x0000` before guest assignment.

The first guest handoff stopped after Windows Boot Manager. The pre-reset trace
and screen were preserved. One virtual-machine reset recovered the guest, and
the official driver then produced this completed trace summary:

| Observation | Result |
|---|---:|
| trace lines | 12,351 |
| BAR reads | 10,368 |
| BAR writes | 186 |
| command rings published | DSP0 through DSP7 |
| response rings published | DSP0 through DSP7 |
| all-eight DMA enable | yes |
| firmware-transition magic | not observed |
| ordinary hard-reset writes | not observed |

This is evidence for fresh host-side framework and transport publication after
the RTC cycle. It is not evidence that this trace contains a fresh firmware
upload because neither the transition magic nor the ordinary hard-reset write
pair appeared.

The output FAT image hash was unchanged before and after the capture. Windows
shut down through ACPI, QEMU exited normally, the endpoint was unbound, and PCI
command was `0x0002`.

## Experiment 054: private allocation selector 1

The Linux reproducer was rebuilt after the host restart and hash locked before
use. It retained the established safety envelope:

- exact PCI and subsystem identity allowlist;
- isolated one-device IOMMU group 16;
- refusal when bus mastering is already enabled;
- exact SHA-256 checks for all captured resource inputs;
- one target DSP and one declared private allocation index;
- a maximum readback length of 430 dwords;
- fixed response deadlines and canary-bounded DMA buffers; and
- explicit restoration, VFIO reset, IOMMU unmap, and unbind.

DSP0 accepted every public resource. The zero-allocation and memory-spec phases
completed, followed by this one-tick Process command:

```text
000b0004 00400002 00000001 0009d00a
```

Both channel responses had valid headers and bounded output. The subsequent
selector-1 readback command and descriptor were consumed immediately. Its
response was valid and contained 150 zero dwords.

All non-target ring indices stayed unchanged, writes remained within declared
response prefixes, every DSP-ready bit remained set, and recovery completed.

## Consequence

The earlier code-shape hypothesis for the `0x0009cf74` allocation is now a
negative result. Its 150 DM32 words numerically correspond to 100 PM48 words,
but shape alone is insufficient. The direct bounded readback returned only
zeros after the complete accepted resource and allocation sequence.

The next high-value target is another allocation selected from the exact
33-entry memory specification, prioritized by nonzero runtime behavior or a
documented execution trace. Repeated zero-allocation scanning without a new
discriminator would add little evidence. The decisive unresolved gate remains
the authenticated inner Bill decoder or an authorized development-object path
that exposes segment records, entry state, and DSP-side relocations.
