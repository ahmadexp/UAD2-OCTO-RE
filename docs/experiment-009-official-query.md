# Experiment 009: official-driver query 026

Status: executed twice on 2026-08-05, no reply, full recovery after each run.

## Objective

Issue one response-backed command whose complete framing was recovered from the
official Windows `UAD2Pcie.sys` driver, then record the five-word response.

## Static provenance

The driver method at virtual-table slot 34 calls its
`_sendDSPCommandWithResponse` helper with these constants:

- command base `0x00260000`, which the helper ORs with `1`;
- expected response header `0x800c0005`;
- four payload words, making a five-dword response DMA buffer.

The helper queues the response buffer before the command, waits up to 2000 ms,
requires the first response word to match, and copies the following four words
to the caller. The operation is therefore classified as a query candidate, not
a block transfer or firmware-management command.

## Bounded sequence

1. Require the exact cold baseline, eight ready DSPs, and VFIO reset support.
2. Map seven pages at IOVA `0x10000000`: four command-ring pages, one
   response-ring page, one four-byte command buffer, and one response buffer.
3. Publish inline command `0x00260001` and a `0x80000005` DMA reference to the
   response buffer. The official helper stores short commands directly in the
   first dword; its separate block-transfer path creates command DMA references.
4. Enable only DSP 0, offer the response entry, then publish the command entry.
5. Poll both hardware read indexes and the response header for no more than
   2000 ms.
6. Reassert DMA reset, clear controls and descriptors, issue an unconditional
   VFIO device reset, and unmap every page.

## Safety boundary

The IOMMU exposes only the seven experiment pages. No firmware image, EEPROM,
flash, DSP reset, interrupt, or unrelated MMIO register is written. Any timeout
or unexpected reply is a failed observation followed by the same reset path.

## Result

Attempt A represented the command as a DMA reference. Attempt B corrected the
framing to the official short-command form, with `0x00260001` inline in the
first ring dword. In both attempts the command hardware read index became `1`,
the response hardware read index remained `0`, the response canary was
unchanged, and all DSPs remained ready. DMA state, controls, and descriptors
were restored, and VFIO reset recovered the cold state after each attempt.

The missing reply does not establish that query 026 is unsupported. Static
analysis subsequently showed that the official driver enables five DSP0
interrupt vectors through BAR0 `0x2204` and arms three of them through
`0x2208`. Experiment 010 tested that prerequisite separately.
