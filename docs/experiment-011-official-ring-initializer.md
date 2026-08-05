# Experiment 011: official DSP 0 ring-initializer order

Status: executed successfully on 2026-08-05, followed by independent recovery
verification.

## Objective

Reproduce only the official driver's command and response ring initializer for
DSP 0. This closes two controlled differences from Experiments 009 and 010:
both rings receive four pages, and both are initialized before any DSP DMA bit
could be enabled.

This experiment does not enable DMA and does not submit query 026.

## Static provenance

The hash-identified driver initializer at `0x14000c47c` performs this sequence
for each ring:

1. Read hardware index `+0x28`.
2. Replace values of 1024 or greater with zero.
3. Write the synchronized index to `+0x24`.
4. Write the same index to `+0x20`.
5. Publish four 64-bit page IOVAs at `+0x00..+0x1c`.

The per-DSP start routine initializes the command ring first and the response
ring second. It enables the DSP DMA bit only after both initializers succeed.

## Preconditions

- Exact endpoint `1a00:0002`, subsystem `1a00:0005` at `0000:03:00.0`.
- Endpoint alone in expected IOMMU group 16.
- Bus mastering disabled before `vfio-pci` is bound.
- VFIO device reset available and Type 1 IOMMU active.
- Host page size exactly 4096 bytes.
- All 256 words across the 16 per-DSP ring windows equal zero.
- BAR `0x2200` equals cold value `0x0001fe00`.
- All eight DSP ready bits are set.
- Eight distinct locked pages mapped into one 32 KiB IOVA window.

## Bounded writes

For DSP 0 command base `0x2000`, then response base `0x2040`:

```text
write synchronized index to base+0x24
write synchronized index to base+0x20
write four 64-bit page IOVAs to base+0x00..base+0x1c
```

Cleanup zeros those 16 descriptor words and four host-index words, verifies all
ring windows are zero, then invokes VFIO device reset before unmapping memory.

The probe contains no call that writes DMA control, interrupt registers, a ring
entry, firmware, flash, EEPROM, DSP reset, or a shared DMA page table.

## Acceptance criteria

1. Both raw hardware indexes are below 1024 or are bounded to zero.
2. All 16 descriptor words and four host-index words read back correctly.
3. Every ring word outside DSP 0's two initialized windows remains zero.
4. All eight canary pages remain byte-for-byte unchanged after 250 ms.
5. BAR `0x2200` remains `0x0001fe00` and every DSP stays ready.
6. Cleanup restores every ring word to zero.
7. VFIO reset succeeds and re-establishes the complete cold baseline.

## Abort conditions

Any changed endpoint, IOMMU, BAR, DMA-control, ring, page-size, reset, or DSP
ready precondition causes refusal before publication. Any unexpected readback,
memory change, ready-bit change, signal, cleanup failure, or reset failure makes
the experiment fail.

## Invocation

The local launcher requires an explicit SSH target:

```bash
export UAD2_TARGET=user@uad2-host
tools/uad2-vfio-official-ring-init.sh
```

The launcher compiles on the target. The root-side wrapper independently checks
the exact endpoint, pre-bind bus-master state, and IOMMU isolation before it
binds `vfio-pci`.

## Interpretation boundary

A successful result establishes only that the official ring-publication order
is accepted while DMA remains disabled. It does not prove that the DSP can use
the pages, that the full-card startup is complete, or that any resident command
will respond.

## Result

Both raw hardware indexes were zero. The probe synchronized all four host-index
writes, published all 16 descriptor words, and read the complete state back
successfully. All eight distinct canary pages remained unchanged, DMA control
remained at cold value `0x0001fe00`, and all DSPs remained ready. No DMA,
interrupt, or command write occurred.

Cleanup restored every ring word to zero and VFIO device reset recovered. A
separate read-only VFIO probe then used no DMA mapping and no MMIO write to
confirm DMA control `0x0001fe00`, all 256 ring-window words zero, DSP 0 at
`0x00000a03`, and DSP 1 through 7 at `0x00000003`.

The structured result is
[`experiment-011-result.json`](experiment-011-result.json), and the separate
capture is
[`experiment-011-independent-recovery.json`](experiment-011-independent-recovery.json).
