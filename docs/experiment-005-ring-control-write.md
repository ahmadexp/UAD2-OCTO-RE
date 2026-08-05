# Experiment 005: isolate DSP 0 ring-control register behavior

Status: executed successfully on 2026-08-05.

## Objective

Determine whether DSP 0 command-ring offset `+0x20` at BAR0 `0x2020` stores a
small nonzero value while its DMA engine remains held in cold reset. Related
public drivers describe this location as a write pointer or descriptor count.
Later official-driver analysis identifies it more precisely as the pending or
notify index written after the host producer index.

## Sequence

1. Require the exact target identity, isolated IOMMU group, no driver, and bus
   mastering disabled before VFIO bind.
2. Attach VFIO with no DMA mappings and verify all 256 ring words are zero,
   DMA control is `0x0001fe00`, and all eight DSPs are ready.
3. Write `1` only to `0x2020`, read it back, and require it to be the only
   nonzero ring word.
4. Wait 250 ms and repeat the read-only invariants.
5. Write `0` to `0x2020`, require all 256 ring words to be zero again, and
   verify identity, DMA control, and DSP-ready state.
6. Issue the proven VFIO reset and require the exact cold state.

## Explicit exclusions

- No DMA mappings or descriptor addresses.
- No write to `+0x24`, now identified as the host write index.
- No DMA-control, command-entry, interrupt, firmware, or DSP-core write.

## Risk boundary

The single target register has unknown semantics on this exact OCTO. The DMA
engines remain held in reset and the VFIO IOMMU has no mapped pages, which
prevents a host-memory transaction. The value is explicitly restored before
the already proven device reset.

## Result

BAR0 `0x2020` read back `1` immediately and after 250 ms. It was the only
nonzero word across all 256 ring-window words. Writing `0` restored the full
zero-ring state, and the subsequent VFIO reset recovered the exact cold state.
