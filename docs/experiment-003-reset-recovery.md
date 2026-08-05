# Experiment 003: validate VFIO reset recovery

Status: executed successfully on 2026-08-05.

## Objective

Prove that the endpoint's advertised VFIO reset capability returns this OCTO to
the captured cold state. This establishes a recovery mechanism before the first
ring pointer, doorbell, or DSP command is submitted.

## Proposed sequence

1. Require exact PCI ID `1a00:0002`, subsystem `1a00:0005`, and isolated IOMMU
   group 16.
2. Require no bound driver and bus mastering disabled.
3. Bind `vfio-pci`, attach `VFIO_TYPE1_IOMMU`, and open the device.
4. Verify FPGA revision `0xa012dc0d`, capabilities `0x00300811`, DMA control
   `0x0001fe00`, 176 zero ring words, and eight ready DSPs.
5. Issue exactly one `VFIO_DEVICE_RESET` ioctl.
6. Poll read-only identity and DSP-ready registers for up to 10 seconds.
7. Require the original FPGA revision and capabilities, DMA control
   `0x0001fe00`, all 176 ring words zero, and all eight ready bits set.
8. Close VFIO, unbind, clear `driver_override`, and verify bus mastering off.

## Explicit exclusions

- No DMA mappings.
- No MMIO writes.
- No ring pointer, doorbell, command, interrupt, or firmware operation.
- No host reboot command.

## Risk boundary

The kernel reports `VFIO_DEVICE_FLAGS_RESET`, and the device is alone on its
downstream bus. Linux exposes the reset method as `bus`, so this ioctl may reset
the PCIe endpoint and its local downstream bus state. If resident firmware does
not self-boot after reset, physical power cycling may be required.

## Result

The exact pre-reset cold baseline matched, the probe issued one
`VFIO_DEVICE_RESET`, and the endpoint matched the complete cold baseline on
the first recovery poll. No DMA memory was mapped and no MMIO write occurred.
An independent read-only VFIO probe then confirmed all 176 documented ring
words were zero and all eight DSPs remained ready. See
`experiment-003-result.json` for the machine-readable record.
