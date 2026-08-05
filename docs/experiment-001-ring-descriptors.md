# Experiment 001: publish empty DSP 0 ring descriptors

Status: executed successfully on 2026-08-04.

## Objective

Determine whether the FPGA accepts IOMMU-backed page addresses for DSP 0's
command and response rings without submitting a command or enabling DMA.

## Preconditions

- Exact endpoint `1a00:0002`, subsystem `1a00:0005`.
- Endpoint alone in IOMMU group 16.
- `vfio-pci` bound and `VFIO_TYPE1_IOMMU` active.
- Bus mastering disabled before the device is opened.
- All eight DSP ready bits set.
- All 176 documented ring-register words equal zero.
- Four command pages and one response page mapped through VFIO.
- Every page filled with a canary and locked in memory.

## Proposed writes

Only DSP 0 descriptor-address registers would be written:

```text
Command ring: BAR0+0x2000 through BAR0+0x201c, four 64-bit IOVAs
Response ring: BAR0+0x2040 and BAR0+0x2044, one 64-bit IOVA
```

The experiment must not write:

- Ring fields `+0x20`, `+0x24`, or `+0x28`.
- DMA control at `0x2200`.
- Interrupt mask or status registers.
- Any doorbell, mailbox, transport, reset, or firmware register.
- Any descriptor for DSP 1 through DSP 7.

## Verification

1. Read back all ten address words and compare them to the IOVAs.
2. Confirm DSP 0 and DSP 1 through DSP 7 retain their ready status.
3. Wait 250 ms and verify all five canary pages remain unchanged.
4. Restore all ten address words to captured zero values.
5. Verify the restored values before unmapping the IOVAs.
6. Close VFIO, unbind, clear `driver_override`, and verify bus mastering is off.

## Abort conditions

- Any precondition differs from the recorded baseline.
- VFIO group is not viable or contains another endpoint.
- Any ring register is nonzero before the experiment.
- Any ready bit clears.
- Any canary byte changes before a doorbell is intentionally introduced.
- A descriptor readback differs from the written IOVA.

## Why this is a separate authorization boundary

This is the first experiment containing MMIO writes. The IOVAs would become
visible to the FPGA, although no doorbell or DMA enable would be written. A
later experiment that enables DMA or submits a ring entry carries a higher risk
and requires another explicit gate.

## Result

All ten address words accepted the five IOVAs and read back correctly. No ring
pointer, doorbell, or DMA-control write occurred. All pages remained unchanged,
all DSPs remained ready, and the ten words restored to zero. An independent
read-only VFIO capture then confirmed all 176 ring-register words were zero.
