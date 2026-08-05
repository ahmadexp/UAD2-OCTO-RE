# Experiment 018: bounded loader-response probes

Status: executed on 2026-08-05 against the isolated OCTO at
`0000:03:00.0`. Three deliberately incomplete inputs were consumed without a
response. Every run preserved DMA bounds, kept all eight DSP ready bits set,
restored the cold ring state, and passed VFIO reset recovery.

## Purpose

The experiment tested the exact short-block framing recovered from the
symbolized macOS `_sendBlock` implementation before considering any complete
firmware or program load. It did not submit the full official HBUT payload.

## Common containment

- Exact PCI identity `1a00:0002`, subsystem `1a00:0005`.
- IOMMU group 16 containing only the card.
- 67 locked 4 KiB pages mapped at a bounded test IOVA.
- 64 pages for all 16 rings, plus separate response, command-header, and
  payload pages.
- Response canary outside the first 16 bytes.
- Three-second timeout.
- Explicit ring, interrupt, and DMA cleanup followed by VFIO device reset.

## Runs

| Input | Framing | Command | Result |
|---|---|---:|---|
| One zero dword | Official chained `_sendBlock` descriptors | `0x00120002` | Command descriptors consumed, no response |
| Official 64-byte HBUT header only | Official chained descriptors | `0x00120011` | Command descriptors consumed, no response |
| One zero dword | Alternate single-buffer hypothesis with embedded `0x80040000` | `0x00120002` | Command descriptor consumed, no response |

The second input contained only the 64-byte header of the hash-locked OCTO
artifact. Its declared 2,558,032-byte body was intentionally absent, so no
executable or complete update image was supplied.

## Interpretation

The official framing is now confirmed at the host-to-FPGA dequeue boundary,
but incomplete loader objects are ignored rather than rejected in the current
resident state. The response descriptor remains pending and its canary remains
unchanged. The alternate single-buffer hypothesis also produces no response.

This does not prove that the loader framing is invalid. It indicates that a
complete recognized framework object, another loader state, or both are
required. A full HBUT transfer remains outside this experiment because the
official updater treats it as a potentially persistent operation.

Machine-readable results are in [`experiment-018-result.json`](experiment-018-result.json).
