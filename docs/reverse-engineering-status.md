# Reverse-engineering completion status

This project has a substantial host-transport reconstruction, but it is not a
full reverse engineering of the UAD-2 OCTO. The distinction matters because a
working ring is not the same as a working DSP runtime, and an outer container
parser is not the same as a decoded executable.

## Requested outcomes

| Requested outcome | Current result | What is still required |
|---|---|---|
| Receive and decode a valid firmware response | Not achieved. Commands dequeue, but no card-written response has been observed. Two valid `Bill` success shapes and HBUT response class are known statically. Operation `0x6f` was eliminated as a candidate because it is host-assembled and never uses a DSP ring. | Enter the official DSP runtime state and capture one lawful ring response. |
| Reproduce the complete shared 4 MiB DMA transport | Resolved for this OCTO profile as inapplicable. Static capability logic and a read-only snapshot show that the optional audio-extension object and BAR `0x8000`/`0xa000` tables are absent. | A different supported device is required to execute that optional path. It should not be fabricated on this OCTO. |
| Identify exact SHARC DSP models and memory maps | ADSP-21469 family and data-sheet map strongly identified. Exact package marking on this physical board is not optically confirmed. | Sharp perpendicular macro photograph of one DSP package marking. |
| Reverse engineer DSP boot and reset control | Host reset masks, ready polling, ring startup, all-eight DMA enable, isolated per-DSP reset, ordinary hard reset, and the firmware-aware `+0x1a8` fork are recovered. Safe transport and per-engine portions were executed; the potentially persistent firmware-aware write was not. DSP ROM boot flow and first framework instruction are not decoded. | Trace a successful official load through hard reset and into the DSP-side boot consumer. |
| Understand firmware and plug-in container formats | Fixed FBUT/GBUT/HBUT wrapper, all 47 artifacts, `Bill` outer header, conditional transform, envelope, allocator, copy limit, and completion parser are recovered. | Decode the opaque HBUT inner payload and one form-zero `Bill` inner core. |
| Determine signing, authentication, relocation, and loading rules | Host-side `Bill` parser performs no cryptographic check. Standard digest, checksum, compression, repetition, and cross-generation tests over 69 unique cores reject several simple layouts. Loader dispatch and status rules are recovered. | Locate DSP-side verification, segment, entry-point, relocation, and reserved-memory consumers. No conclusion about signing can yet be made. |
| Load a harmless DSP0 program | Not attempted, by design. No target-specific executable or entry ABI is proven. | Valid framework response, decoded OCTO-compatible core, bounded output buffer, timeout, and recovery oracle. |
| Implement host-to-DSP buffers and completion handling | Ring DMA and resource completion ABI are reconstructed. Public compute calls remain fail-closed. | One real program load and one bounded buffer exchange before enabling UAPI job calls. |
| Build reusable Linux driver and userspace API | Bounded kernel transport, versioned userspace API, identity checks, coherent ring allocation, start, status, reset, and stop exist. Program/buffer/job/wait operations return `-EOPNOTSUPP`. | Enable capabilities only after their hardware contracts pass. |
| Validate isolation and recovery across all eight DSPs | Empty-transport reset isolation and exact recovery pass for every DSP. | Repeat with a proven program and per-engine buffer fault injection, then complete the 56-case matrix. |

## Material progress in the latest pass

- Closed the complete operation-`0x6f` path across client, system, and PCIe
  drivers. The 168-byte record is assembled from cached host state and BAR
  MMIO, not returned by a DSP.
- Tied `-0x5c` exactly to host object field `+0x0c40`, including constructor,
  start, initialization, and stop writes, and mapped the labeled record fields
  to their exact BAR sources.
- Proved that firmware operation `0x69` selects one dedicated target method and
  does not automatically bracket itself with operations `0x67` and `0x68` in
  the common dispatcher.
- Verified both updater callers and the complete `LoadBinFile` magic branch.
  Single-device and multi-device paths dispatch `FBUT`, `GBUT`, or `HBUT`
  directly through one firmware-update method with no automatic pre-operation
  or post-operation.
- Recovered all 13 `CPcieDSP::GetProperty` switch cases and the property-size
  table. Resource properties 6, 7, and 8 are local BAR or cached-state reads,
  not hidden DSP queries. Property 6 exactly matches Experiment 020's eleven
  pool words.
- Added a dependency-free, hash-locked Mach-O verifier for six symbols, 21
  instruction signatures, the property switch, and its size table.
- Recovered a cross-platform firmware-aware hard-reset fork. Ordinary reset
  samples ready state and pulses BAR `+0x221c`; after `LoadFirmware` sets its host
  flag, hard reset instead writes `0x0be0deaf` to DSP0 `+0x1a8`. Device-side
  semantics remain intentionally unresolved and unexecuted.
- Recovered 4 KiB resource-copy chunks, ten ordinary 600 ms waits, operation
  13 status polling, intermediate success, final success, and all recognized
  final low-code mappings.
- Aggregated all 87 official `Bill` instances without emitting payload bytes.
  The opaque prefixes are 32, 48, or 96 bytes, not one fixed length.
- Rejected standard clear digests, CRC-32, Adler-32, common compression
  wrappers, aligned repeated blocks, and clear cross-generation similarity in
  addition to the four direct SHA-256 layouts.
- Matched five public Apollo captures to five official cabinet resources. Every
  byte after the outer resource ID is identical; only the resource ID high byte
  differs. This supplies useful semantic correlations but no OCTO entry ABI.
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
impossible. Operation `0x6f` is no longer on the critical path. The next
highest-value artifact is a lawful official update trace or the first DSP-side
consumer of an HBUT or form-zero `Bill` core.
