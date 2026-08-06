# Experiments 033 and 034: resource lifecycle and allocation preflight

Status: executed on 2026-08-06. The exact pool-zero unload commands were
consumed, but the resident DSP runtime service did not resume responding.
Consequently, the allocation and memory-spec phase was never submitted.

## Static lifecycle semantics

The hash-locked public driver closes two command meanings that were only
capture-correlated before this experiment:

- A resource mapping with no functional payload emits `0x00080004`, mapped
  offset, zero, and length. This is the private-resource zeroing command.
- Removing an ordinary pool-zero mapping emits `0x00030002`, resource ID,
  zero, and zero.

The same driver builds the already recovered memory-spec update as
`0x00150000 | (2 * count - 1)`. The captured RealVerb object has 32 address
pairs, so its exact header is `0x00150041` and its total length is 65 dwords.

These facts are now covered by 11 hash-locked signatures in:

```bash
python3 tools/inspect_program_runtime_abi.py /path/to/uad2.kext
```

## Experiment 033: exact cleanup

After Experiment 032, a second resource pass consumed the first `0x12b`
command but did not consume its response descriptor. This showed that VFIO
device reset did not restore the prior service state.

The cleanup mode submitted only the 13 official pool-zero unmap commands in
the captured order. Every command was consumed in 1 ms. All seven non-target
ring-index sets remained unchanged, all DMA pages remained unchanged, all DSPs
reported ready, and explicit restore plus VFIO reset passed.

Command consumption is not treated as proof that the DSP-side resource table
was cleared. The next reload still did not receive a response.

## Experiment 034: allocation preflight

The allocation mode was constructed but remained fail-closed. Its intended
sequence is:

1. accept all 13 exact program resources;
2. consume the 33 exact `0x00080004` private-resource zero commands;
3. consume the hash-locked 65-dword `0x00150041` address-patch object;
4. issue the fixed four-dword readback only after all earlier gates pass.

The preflight stopped at step 1. The first `0x12b` command was consumed, its
response descriptor remained unconsumed for the 600 ms deadline, and no
private-resource, memory-spec, or readback command was published. Query 026
was then also consumed without a response. The already validated isolated
reset sequence passed across all eight DSPs, but query 026 remained
unanswered afterward.

This establishes a new operational boundary: the complete resource pass and
following unanswered readback can leave the resident service unavailable even
though all ready bits, ring reset, VFIO reset, and per-DSP reset checks pass.
A fresh official plug-in activation is required before Experiment 034 can be
retried. The public wrapper now requires the explicit environment value
`UAD2_ALLOW_ONE_SHOT_RESOURCE_PASS=YES_I_ACCEPT_OFFICIAL_REACTIVATION_MAY_BE_REQUIRED`
for load or allocation modes. Cleanup mode remains available without that
acknowledgement.

No general-purpose program ran, and no Linux buffer or job capability is
enabled by this result.

The combined sanitized record is
[`result.json`](data/experiment-033-034-resource-lifecycle-and-allocation/result.json).
