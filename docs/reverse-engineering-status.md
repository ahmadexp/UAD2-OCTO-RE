# Reverse-engineering completion status

This project has a substantial host-transport reconstruction, but it is not a
full reverse engineering of the UAD-2 OCTO. The distinction matters because a
working ring is not the same as a working DSP runtime, and an outer container
parser is not the same as a decoded executable.

## Requested outcomes

| Requested outcome | Current result | What is still required |
|---|---|---|
| Receive and decode a valid firmware response | Not achieved. Commands dequeue, but no card-written response has been observed. Two valid `Bill` success shapes and HBUT response class are known statically. | Enter the official runtime state and capture one lawful response. |
| Reproduce the complete shared 4 MiB DMA transport | Resolved for this OCTO profile as inapplicable. Static capability logic and a read-only snapshot show that the optional audio-extension object and BAR `0x8000`/`0xa000` tables are absent. | A different supported device is required to execute that optional path. It should not be fabricated on this OCTO. |
| Identify exact SHARC DSP models and memory maps | ADSP-21469 family and data-sheet map strongly identified. Exact package marking on this physical board is not optically confirmed. | Sharp perpendicular macro photograph of one DSP package marking. |
| Reverse engineer DSP boot and reset control | Host reset masks, ready polling, ring startup, all-eight DMA enable, and isolated per-DSP reset are recovered and executed. DSP ROM boot flow and first framework instruction are not decoded. | DSP-side boot consumer analysis or a trace from reset into framework startup. |
| Understand firmware and plug-in container formats | Fixed FBUT/GBUT/HBUT wrapper, all 47 artifacts, `Bill` outer header, conditional transform, envelope, allocator, copy limit, and completion parser are recovered. | Decode the opaque HBUT inner payload and one form-zero `Bill` inner core. |
| Determine signing, authentication, relocation, and loading rules | Host-side `Bill` parser performs no cryptographic check; four direct SHA-256 layouts fail across all 87 instances. Loader dispatch and status rules are recovered. | Locate DSP-side verification, segment, entry-point, relocation, and reserved-memory consumers. No conclusion about signing can yet be made. |
| Load a harmless DSP0 program | Not attempted, by design. No target-specific executable or entry ABI is proven. | Valid framework response, decoded OCTO-compatible core, bounded output buffer, timeout, and recovery oracle. |
| Implement host-to-DSP buffers and completion handling | Ring DMA and resource completion ABI are reconstructed. Public compute calls remain fail-closed. | One real program load and one bounded buffer exchange before enabling UAPI job calls. |
| Build reusable Linux driver and userspace API | Bounded kernel transport, versioned userspace API, identity checks, coherent ring allocation, start, status, reset, and stop exist. Program/buffer/job/wait operations return `-EOPNOTSUPP`. | Enable capabilities only after their hardware contracts pass. |
| Validate isolation and recovery across all eight DSPs | Empty-transport reset isolation and exact recovery pass for every DSP. | Repeat with a proven program and per-engine buffer fault injection, then complete the 56-case matrix. |

## Material progress in the latest pass

- Assigned six fields in the official 168-byte system-information record.
- Proved that firmware operation `0x69` selects one dedicated target method and
  does not automatically bracket itself with operations `0x67` and `0x68` in
  the common dispatcher.
- Recovered 4 KiB resource-copy chunks, ten ordinary 600 ms waits, operation
  13 status polling, intermediate success, final success, and all recognized
  final low-code mappings.
- Aggregated all 87 official `Bill` instances without emitting payload bytes.
  The opaque prefixes are 32, 48, or 96 bytes, not one fixed length.
- Rejected four simple direct SHA-256 layouts across every instance.
- Reconfirmed the live endpoint at `0000:03:00.0`, subsystem `1a00:0005`, in
  isolated IOMMU group 16 with no driver bound. The check performed no MMIO,
  DMA, reset, or command operation.

## Why the remaining hardware actions are gated

A speculative HBUT submission can cross a persistent firmware boundary. A
speculative `Bill` submission lacks a proven executable target, entry point,
and memory-safety contract. Either experiment could produce an ambiguous
timeout while adding card or host risk. The project therefore requires a
discriminating static or official-trace result before another write.

This is an evidence gate, not an assertion that the remaining work is
impossible. The next highest-value artifact is a lawful official operation
`0x6f` or update trace, followed by the first DSP-side consumer of a form-zero
`Bill` core.
