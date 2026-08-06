# Experiment 031: post-load bounded resource readback

Status: executed on 2026-08-06. The exact authenticated `0x12b` resource was
accepted on DSP0. A following fixed-address, four-dword readback command was
consumed, but it produced no response within the six-second deadline.

## Static prerequisite

The exact public x86-64 driver at commit
`910a8f413d33bc3489d0da8fc613870153ccb4f2` exposes the complete builder for
plug-in readback descriptors:

| Command word | Meaning |
|---:|---|
| `0x000c0004` | four-dword resource readback request |
| word 1 | mapped resource base plus the low 24 bits of the resource spec |
| word 2 | requested dword count |
| word 3 | original resource spec, whose high byte selects a resource |

The paired response descriptor is exactly `requested_dwords + 2` dwords. The
same driver also exposes a separate runtime relocation mechanism. Its
allocation record is `0x0af0` bytes, memory-spec entries begin at `0x188` and
are 16 bytes each, and readback specs begin at `0x98c` and are 8 bytes each.
Command class `0x00150000` resolves each spec as a mapped resource plus a
low-24-bit offset and associates that value with the mapped private-resource
destination.

These findings are reproducible with:

```bash
python3 tools/inspect_program_runtime_abi.py /path/to/uad2.kext
```

The verifier is hash locked and emits only structure and control-flow facts.

## Live method

The Linux probe adopted the resident official framework under fresh VFIO and
IOMMU mappings. It submitted the exact hash-locked `0x12b` command target and
posted its four-dword completion buffer. Immediately afterward it queued:

```text
000c0004 000e0000 00000004 00000000
```

The address is the already observed mapped allocation for `0x12b`. The probe
does not accept an arbitrary address or length. A separate six-dword response
buffer was filled with `0xa5` canaries. All remaining bytes in the 67-page
mapping were snapshotted and checked after the deadline.

## Result

The resource response was exact:

```text
80070004 00000000 0000012b 00010073
```

Both command descriptors were consumed. The readback response descriptor was
not consumed, its six dwords remained `0xa5a5a5a5`, and no out-of-prefix DMA
write occurred. All eight DSP ready bits remained set. Explicit restore and
VFIO device reset both returned the card to the clean baseline.

This is a safe negative result. It proves that `0x000c0004` is not a
standalone decoded-memory oracle after isolated resource acceptance. The
official driver appends it to a plug-in `Process` transaction, so a valid
process command or complete plug-in activation is now a demonstrated
prerequisite candidate. Varying the address, length, or resource selector
without that prerequisite is not justified.

The sanitized result is
[`result.json`](data/experiment-031-post-load-readback/result.json).
