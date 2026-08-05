# Experiment methodology

The project advances only when a smaller experiment establishes the safety
property required by the next one. Every mutating run has an explicit target,
preconditions, allowed writes, timeout, acceptance criteria, and recovery path.

## Safety levels

1. Read PCI configuration and sysfs metadata only.
2. Read a small allowlist of MMIO registers with bus mastering disabled.
3. Write one captured register value while DMA remains unavailable.
4. Map bounded pages through a VFIO IOMMU domain before enabling DMA.
5. Publish empty rings before publishing any entry.
6. Submit only a statically recovered, response-backed query candidate.
7. Consider firmware or program loading only after the complete startup and
   recovery paths are understood.

## Required preconditions for a mutating probe

- Exact PCI and subsystem identity matches the tested OCTO.
- BAR0 size matches 64 KiB.
- The endpoint has no driver before the wrapper begins.
- PCI bus mastering is off before VFIO owns the endpoint.
- The IOMMU group contains only the target endpoint.
- The VFIO device reports reset support.
- DMA control, ring words, and DSP-ready values match the captured cold state.
- Every possible device IOVA resolves inside an experiment-owned mapping.

Any mismatch is a refusal, not a reason to weaken a check.

## Cleanup order

For DMA experiments, cleanup first reasserts cold DMA control. It then clears
interrupt enables, ring indexes, and page descriptors as applicable. The probe
requests VFIO device reset even after a timeout or unexpected result, unmaps
the complete IOVA range, closes VFIO, unbinds `vfio-pci`, and clears
`driver_override`.

## Evidence discipline

- Preserve raw values and hashes in JSON.
- Separate an observation from its interpretation.
- Record negative results and timeouts.
- Treat public register names as hypotheses until driver or hardware evidence
  establishes their behavior.
- Revisit earlier conclusions when stronger evidence appears.
- Do not infer successful command decoding from producer or consumer movement
  alone.

Experiment 008 demonstrates the last two rules. Its original direct-fetch
claim was withdrawn after the ring implementation was recovered more exactly.

## Publication boundary

Do not commit board serials, credentials, private host addresses, proprietary
driver binaries, firmware images, installer payloads, or vendor symbols. Hashes
and independently written protocol descriptions are sufficient to identify
the analyzed material.
