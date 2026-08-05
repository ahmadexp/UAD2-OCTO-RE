# Experiment 006: isolate DSP 0 ring offset +0x24 behavior

Status: executed successfully on 2026-08-05.

## Objective

Determine whether DSP 0 command-ring offset `+0x24` at BAR0 `0x2024` stores a
small nonzero value while its DMA engine remains held in cold reset. Public
drivers provisionally identified this location as a doorbell or ring size.
Later official-driver analysis identifies it as the host write index.

## Sequence

The sequence is identical to Experiment 005 except that the sole target is
`0x2024`: write `1`, verify it is the only changed ring word, wait 250 ms,
restore `0`, verify all 256 words are zero, then issue one proven VFIO reset.

## Explicit exclusions

- No DMA mappings, descriptor addresses, or ring entry.
- No write to the `+0x20` control tested in Experiment 005.
- No DMA-control, interrupt, firmware, or DSP-core write.

## Risk boundary

If `+0x24` is a doorbell, the DMA engine is still held in cold reset and the
VFIO IOMMU has no mapped memory. The test cannot authorize a host-memory DMA
transaction. The target is restored before a proven endpoint reset.

## Result

BAR0 `0x2024` read back `1` immediately and after 250 ms. It was the only
nonzero word across all 256 ring-window words. Writing `0` restored the full
zero-ring state, and the subsequent VFIO reset recovered the exact cold state.
