# Experiment 030: authenticated loader isolation across eight DSPs

The exact accepted `0x12b` object was submitted independently to DSP0 through
DSP7. All eight engines returned the same success in one millisecond.

For each target, the probe initialized separate command and response rings,
used the target’s physical interrupt bits, and recorded every non-target ring
index before cleanup. The complete 66-page IOMMU region was canary checked.

## Result

| DSP | accepted | target latency | seven non-target index sets unchanged |
|---:|---|---:|---|
| 0 | yes | 1 ms | yes |
| 1 | yes | 1 ms | yes |
| 2 | yes | 1 ms | yes |
| 3 | yes | 1 ms | yes |
| 4 | yes | 1 ms | yes |
| 5 | yes | 1 ms | yes |
| 6 | yes | 1 ms | yes |
| 7 | yes | 1 ms | yes |

Every trial also preserved all eight ready bits, confined writes to the
four-word target response, and passed explicit restore plus VFIO reset.

This proves program-resource loader isolation and target selection across all
eight DSPs. It is not program-level execution isolation: no module entry point
was activated, no heartbeat ran, and no deliberate timeout or out-of-bounds
program fault was induced.

See [`result.json`](data/experiment-030-eight-dsp-bill-isolation/result.json).
