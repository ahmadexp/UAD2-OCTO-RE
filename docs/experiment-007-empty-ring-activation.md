# Experiment 007: activate an empty DSP 0 command ring at pointer zero

Status: executed successfully on 2026-08-05.

## Objective

Combine the validated descriptor publication, DSP 0 DMA-engine enable, and
ring-control writes while the ring contains no command entry and both control
values remain zero. This tests whether an empty activation causes any bounded
DMA-page or device-state change.

## Sequence

1. Require the exact cold baseline and isolated VFIO target.
2. Map four zero-filled command pages and one response-canary page in the VFIO
   IOMMU domain, then publish only their five descriptor addresses.
3. Pulse the DMA resets and enable only the global and DSP 0 engine bits.
4. Write command-ring `+0x24 = 0`, then `+0x20 = 0`, matching the ordering in
   public ring-service code but authorizing zero entries.
5. Wait 250 ms and require unchanged pages, ring registers, and DSP-ready state.
6. Restore cold DMA control and all descriptor words to zero, unmap memory, and
   run the proven VFIO reset as an independent recovery step.

## Explicit exclusions

- No nonzero pointer or doorbell value.
- No command or response entry.
- No interrupt, firmware, DSP-core, or PCI configuration write.

## Risk boundary

The FPGA ring engine is enabled while all authorized pointers remain zero.
Every published IOVA resolves only to one of five locked test pages, and the
VFIO IOMMU contains no other mapping. The empty-ring state is restored before
the proven endpoint reset.

## Result

The five descriptors and three DMA-control transitions read back exactly. The
zero-valued `+0x24` and `+0x20` writes caused no change to any mapped page,
published ring register, or DSP-ready value. DMA control and descriptors were
restored, and the independent VFIO reset recovered the exact cold state.
