# Experiment 002: release and enable only the DSP 0 DMA ring engine

Status: executed successfully on 2026-08-05.

## New passive finding

The cold DMA control register at `BAR0+0x2200` is stable at `0x0001fe00`.
Public driver analysis identifies bits 16:9 as eight reset strobes, bits 8:1 as
eight per-engine enables, and bit 0 as the global enable. Thus the cold value is
consistent with every DMA engine held in reset and all enable bits clear.

Other stable nonzero values in the surveyed control neighborhood are:

```text
0x2218 = 0xa012dc0d
0x2228 = 0x00000040
0x2234 = 0x00300811
```

## Objective

Test whether the DSP 0 host ring engine can be released from reset and enabled
while its empty, IOMMU-backed pages are published, without submitting a command
or allowing access outside those pages.

## Proposed sequence

1. Revalidate the Experiment 001 preconditions.
2. Map four zero-filled command pages and one response canary page.
3. Publish only the ten DSP 0 descriptor-address words.
4. Write `0x0001fe01` to `0x2200`, asserting reset with global enable set.
5. Write `0x00000001` to `0x2200`, releasing reset with engines disabled.
6. Write `0x00000003` to `0x2200`, enabling only global and DSP 0.
7. Wait 250 ms without writing a pointer, doorbell, or ring entry.
8. Verify all pages unchanged, all DSPs ready, and all ring fields except the
   published addresses still zero.
9. Restore `0x2200` to the exact captured cold value `0x0001fe00`.
10. Restore the ten descriptor words to zero and verify them.
11. Unmap IOVAs, close VFIO, unbind, and verify bus mastering is off.

## Explicit exclusions

- No writes to ring `+0x20`, `+0x24`, or `+0x28`.
- No command or response entry.
- No doorbell.
- No interrupt-register access.
- No DSP-core reset, PCI reset, transport start, or firmware upload.
- No enable bit for DSP 1 through DSP 7.

## Risk boundary

This experiment writes the global DMA control register and pulses all eight DMA
reset strobes before enabling only DSP 0. The IOMMU limits any unexpected access
to five mapped pages, but this is a higher-risk boundary than Experiment 001.

## Result

All three transitions read back exactly as proposed. No page or ring field
changed, all DSPs remained ready, DMA control restored to `0x0001fe00`, and all
ten descriptor words restored to zero. An independent read-only VFIO capture
confirmed the cold DMA value and 176 zero ring words.
