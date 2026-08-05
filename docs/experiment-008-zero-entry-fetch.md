# Experiment 008: publish one all-zero DSP 0 ring entry

Status: executed successfully on 2026-08-05.

## Objective

Publish one DSP 0 command-queue entry while its first 16 bytes are entirely
zero. Observe the pending/notify index at `+0x20`, host write index at `+0x24`,
and hardware read index at `+0x28`.

## Sequence

1. Establish the exact cold baseline and five-page VFIO IOMMU sandbox used in
   Experiment 007.
2. Publish the four command-page descriptors and one response-page descriptor.
3. Pulse DMA reset and enable only DSP 0.
4. Confirm entry zero is all zeros, then write `+0x24 = 1` followed by
   `+0x20 = 1`.
5. Wait 250 ms, capture the three control words, require all other ring state
   and all five pages to remain unchanged, and require all DSPs ready.
6. Reassert cold DMA reset, restore both controls and all descriptors to zero,
   then issue one unconditional VFIO reset before unmapping the pages.

## Explicit exclusions

- No recognized or nonzero command opcode.
- No DMA-reference entry or address inside a ring entry.
- No interrupt, firmware, or DSP-core write.

## Risk boundary

An all-zero entry is still an unknown opcode on this exact card. Any memory
transaction is restricted to the five test pages by the VFIO IOMMU. Cleanup
reasserts DMA reset first, and the tool invokes the proven device reset even if
the observation does not match expectations.

## Result

The pending/notify and host write indexes both retained `1`, while the hardware
read index advanced from `0` to `1` within 250 ms. All five pages remained byte
for byte unchanged, all other ring state remained within the published
descriptor contract, and all DSPs stayed ready. Cleanup restored DMA control,
controls, and descriptors before a successful VFIO reset.

Official-driver analysis performed afterward confirms the three index roles.
The observation is strongly consistent with hardware dequeuing entry zero, but
the all-zero entry and unchanged memory do not independently prove that its 16
bytes were fetched or decoded. The original stronger claim of direct proof is
therefore withdrawn.
