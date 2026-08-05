# Experiment 013: full OCTO empty startup

Status: executed successfully on 2026-08-05, followed by independent recovery
verification.

## Objective

Reproduce the official non-audio startup path across all eight DSPs without
submitting a command: 64 distinct IOMMU pages, both four-page rings per DSP,
compressed callback interrupt mapping, and sequential DSP DMA enables.

## Safety boundary

The probe validated the exact PCI and subsystem IDs, isolated IOMMU group,
64 KiB BAR, cold ring state, ready bits, page size, and reset support. It did
not write either optional audio table, load firmware, or submit a ring entry.
Every DMA page contained a canary and every descriptor and control write had an
explicit inverse cleanup operation.

## Result

All 16 rings were published, all 64 pages remained unchanged, and DMA control
reached `0x000001ff`. All eight DSPs stayed ready and interrupt enable remained
zero. Explicit cleanup restored the cold register state, VFIO reset succeeded,
and a separate read-only probe observed all 256 ring-window words at zero.

The result establishes complete OCTO ring publication, DMA-enable sequencing,
and all-eight recovery. It does not establish resident firmware execution.

See [`experiment-013-result.json`](experiment-013-result.json) and
[`experiment-013-independent-recovery.json`](experiment-013-independent-recovery.json).

