# Paper draft

This directory contains a venue-neutral, pre-results manuscript for the study:

> When GPUs Do Not Perform Best: Evaluating UAD-2 OCTO DSPs for Low-Latency Streaming Workloads

The draft deliberately does not claim that the UAD-2 OCTO is faster than a GPU.
The current repository completes one bounded authorized UAD buffer job on all
eight DSPs, but has no user-authored general-purpose UAD program and no
completed UAD-versus-GPU comparison. The manuscript defines the workloads,
baselines, metrics, sampling plan, and decision rule needed to make a defensible
comparison after custom execution is available.

The current evidence also includes a valid Process-coupled private-resource
readback, a complete 430-dword runtime-object snapshot, and an exact match
between all 32 nonzero words and the host relocation destinations. A bounded
public Bill readback negative control remains unanswered. These findings close
the private runtime layout, but do not expose a user-authored instruction
stream.

Build from this directory:

```sh
make
```

The generated PDF is written to:

```text
output/pdf/when-gpus-do-not-perform-best-study-design.pdf
```

Before treating the manuscript as a results paper, all of these gates must be
met:

1. Decode one accepted OCTO resource into segments, relocations, and an entry
   point without publishing proprietary bytes.
2. Execute a user-authored harmless DSP0 heartbeat with bounded output.
3. Complete and verify the affine transform.
4. Implement matched optimized GPU and CPU baselines.
5. Record the exact GPU model and full host configuration.
6. Collect correctness, latency, deadline, throughput, and power data using the
   protocol in `main.tex`.
7. Replace the evidence-status notice and results-status section with measured
   results, including losses and ties.
