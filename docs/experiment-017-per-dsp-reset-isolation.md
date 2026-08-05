# Experiment 017: per-DSP reset isolation

Status: executed successfully on 2026-08-05, followed by independent read-only
recovery verification.

## Objective

Validate the official DSP-specific reset and re-enable sequence independently
for all eight engines while every ring is empty and IOMMU-contained.

## Sequence

After Experiment 013's full startup, each DSP was tested in isolation:

1. Clear DMA enable bit `dsp + 1` while preserving every other engine.
2. Pulse reset bit `dsp + 9` for one MMIO write.
3. Verify the disabled DMA shadow and all eight ready bits.
4. Restore only enable bit `dsp + 1`.
5. Verify live shadow `0x000001ff` before moving to the next DSP.

The procedure submitted no ring command and never accessed the optional audio
tables, firmware, flash, or EEPROM.

## Result

Every DSP passed. The accumulated enable-bit mask was `0x000001fe`. All 64 DMA
pages stayed byte-for-byte unchanged, all DSPs remained ready, and interrupt
enable remained zero. Cleanup and VFIO reset both succeeded.

An independent probe then made no DMA mapping and no MMIO write. It observed
DMA control `0x0001fe00`, all 176 ordinary ring words zero, and all eight stable
ready values.

See [`experiment-017-result.json`](experiment-017-result.json) and
[`experiment-017-independent-recovery.json`](experiment-017-independent-recovery.json).

