# Experiment 027: valid Linux response from the resident framework

This experiment closes the valid-response transport blocker. Linux adopted a
framework state initialized by the unmodified official driver and received a
card-written response to query `0x00260001` in one millisecond.

## State bridge

The reference VM initialized the DSP framework and all eight engines, then
exited while the endpoint was held in DMA cold reset. Its IOMMU mappings no
longer existed. The Linux VFIO probe therefore:

1. required all eight ready bits, DMA control `0x0001fe00`, and interrupts off;
2. cleared only stale ring page publications and host pointers while DMA was
   reset;
3. mapped and locked its own 66-page IOMMU region;
4. published fresh command and response rings;
5. submitted one read-only query to DSP0;
6. restored cold DMA state, cleared the rings, issued VFIO reset, and unbound.

This is an adopted-state path. It does not claim that the Linux driver can
cold-boot the proprietary framework.

## Result

| Check | Observation |
|---|---|
| command | `0x00260001` |
| response header | `0x800c0005` |
| response length | 5 dwords |
| completion latency | 1 ms |
| command and response consumed | yes |
| writes outside response prefix | none |
| all eight DSPs ready | yes |
| explicit restore and VFIO reset | both passed |

The four-dword response body is device-specific and remains private. Its
SHA-256 is recorded in the sanitized result so a later capture can be compared
without publishing the receipt.

The result proves the complete ring, IOMMU, doorbell, interrupt-mask, response,
and cleanup path for a real framework service. It does not establish arbitrary
program loading or a compute ABI.

See [`result.json`](data/experiment-027-post-official-linux-response/result.json).
