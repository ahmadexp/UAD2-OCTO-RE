# Experiment 010: official query 026 with DSP 0 interrupt gates

Status: executed once on 2026-08-05, no reply, full independent recovery.

## Objective

Repeat the corrected inline form of official query 026 while mirroring the
official driver's DSP 0 interrupt-enable sequence.

## Static provenance

The official interrupt manager maps five vectors per DSP. DSP 0 uses command
ring vector 0, response ring vector 1, and callback vectors 2, 3, and 4. During
initialization the driver arms vectors 2 through 4 through BAR0 `0x2208` and
accumulates their enable mask in `0x2204`. Ring flushes add response vector 1
and command vector 0 without arming them.

The resulting bounded sequence is:

```text
write 0x2208 = 0x04; write 0x2204 = 0x04
write 0x2208 = 0x08; write 0x2204 = 0x0c
write 0x2208 = 0x10; write 0x2204 = 0x1c
publish response;       write 0x2204 = 0x1e
publish command;        write 0x2204 = 0x1f
```

## Safety boundary

The experiment retained Experiment 009's seven-page VFIO IOMMU sandbox,
two-second timeout, exact target checks, DSP 0-only DMA enable, and unconditional
VFIO reset. Cleanup first restored cold DMA control, then cleared `0x2204` and
`0x2208`, ring controls, and page descriptors. No firmware, flash, EEPROM, DSP
boot, or additional command was written.

## Result

The interrupt enable register read back `0x0000001f`. The command hardware read
index became `1`, while the response hardware read index remained `0` for the
full 2000 ms. All five response words retained the `0xa5a5a5a5` canary, all
DSPs remained ready, and command/ring memory outside the response area remained
unchanged. Cleanup restored DMA, descriptors, and interrupt enables, and the
probe's VFIO reset recovered the cold state.

An independent read-only VFIO probe then found DMA control `0x0001fe00`, all
176 sampled ring words zero, and all eight DSPs ready. No DMA mapping or MMIO
write was used for that verification.

The official interrupt gates are therefore not the only missing prerequisite.
Later static analysis found that the driver initializes four response-ring
pages, initializes both rings before enabling the DSP DMA bit, and performs
additional device-level setup first. No further recognized command should be
sent until that earlier device and DSP initialization sequence is recovered
and bounded.
