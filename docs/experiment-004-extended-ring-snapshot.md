# Experiment 004: capture extended ring-register words

Status: executed successfully on 2026-08-05.

## Objective

Read the five previously unobserved words at offsets `+0x2c`, `+0x30`,
`+0x34`, `+0x38`, and `+0x3c` in both ring windows for all eight DSPs. This
completes a 64-byte passive snapshot of each command and response ring bank
before any pointer or doorbell write.

## Sequence

1. Require the exact OCTO PCI and subsystem IDs, isolated IOMMU group 16, no
   bound driver, and bus mastering disabled.
2. Bind `vfio-pci` and map BAR0 read-only.
3. Verify the documented 176 ring words, DMA control, and eight DSP-ready
   locations while reading each extended word exactly once.
4. Unbind VFIO and verify bus mastering remains disabled.
5. Run Experiment 003 once to restore the proven cold state in case any
undocumented register has read-to-clear semantics.

## Result

All 80 newly observed words were zero. Combined with the prior 176-word
capture, every 32-bit word in all sixteen 64-byte ring windows was zero. DMA
control remained `0x0001fe00`, and all eight DSP-ready values remained stable.
The follow-up VFIO reset recovered the exact cold state on its first poll.

## Explicit exclusions

- No MMIO writes.
- No DMA mappings.
- No interrupt reads outside the per-DSP ring banks.
- No command, pointer, doorbell, or firmware operation.

## Risk boundary

The semantics of the 80 new words are unknown, so a read could theoretically
clear or acknowledge device state. The endpoint has no active host driver or
audio workload, and the proven reset-recovery experiment follows immediately.
