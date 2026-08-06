# Reverse-engineering completion status

This project has a substantial host-transport reconstruction, but it is not a
full reverse engineering of the UAD-2 OCTO. The distinction matters because a
working ring is not the same as a working DSP runtime, and an outer container
parser is not the same as a decoded executable.

## Requested outcomes

| Requested outcome | Current result | What is still required |
|---|---|---|
| Receive and decode a valid firmware response | Partially achieved. Four card-written `0x80030302` responses and two exact `0x80070004` `Bill` successes are structurally decoded. The HBUT completion itself stayed zero, and the 768 authorization-state meanings remain unnamed. | Name the four table states and capture the later response that turns a complete plug-in load into success or `-38`. |
| Reproduce the complete shared 4 MiB DMA transport | Resolved for this OCTO profile as inapplicable. Static capability logic and a read-only snapshot show that the optional audio-extension object and BAR `0x8000`/`0xa000` tables are absent. | A different supported device is required to execute that optional path. It should not be fabricated on this OCTO. |
| Identify exact SHARC DSP models and memory maps | Achieved at the component level. The high-resolution photograph confirms `ADSP-21469 KBCZ-00` on all eight SHARCs, and the family data-sheet map is transcribed. | Recover the board-specific runtime reservations and loader map; the package marking does not independently prove the `-3` or `-4` speed grade. |
| Reverse engineer DSP boot and reset control | Host reset masks, ready polling, ring startup, all-eight DMA enable, isolated per-DSP reset, ordinary hard reset, and the firmware-aware `+0x1a8` fork are recovered. The official update, RTC cold cycle, post-update startup, and ordinary shutdown reset are traced. The magic fork was not observed or synthesized. DSP ROM boot flow and first framework instruction are not decoded. | Trace a plug-in program load into the DSP-side boot or overlay consumer. |
| Understand firmware and plug-in container formats | Fixed FBUT/GBUT/HBUT wrapper, all 47 artifacts, full official HBUT transport, `Bill` outer header, conditional transform, envelope, allocator, copy limit, and completion parser are recovered. Two official form-zero resources are accepted dynamically. | Decode the opaque HBUT inner payload and one accepted form-zero `Bill` inner core. |
| Determine signing, authentication, relocation, and loading rules | Host-side `Bill` parser performs no cryptographic check. Two byte-for-byte form-zero objects received DSP-side intermediate success at observed pool offsets. Standard digest, checksum, compression, repetition, and cross-generation tests over 69 unique cores reject several simple layouts. | Locate the DSP-side verification, segment, entry-point, relocation, and inner reservation consumers. Acceptance of two official objects does not establish the signing rule. |
| Load a harmless DSP0 program | Partial official load only. RealVerb-Pro instantiated and at least two resources were accepted, but the host disabled it on `-38`; no running DSP workload or output was confirmed. | Identify the first all-zero response in the resource chain, decode the target ABI, then load a bounded heartbeat with a recovery oracle. |
| Implement host-to-DSP buffers and completion handling | Ring DMA, authorization responses, and resource completion are reconstructed and observed. Public compute calls remain fail-closed. | One complete program load and one bounded buffer exchange before enabling UAPI job calls. |
| Build reusable Linux driver and userspace API | Bounded kernel transport, versioned userspace API, identity checks, coherent ring allocation, start, status, reset, and stop exist. Program/buffer/job/wait operations return `-EOPNOTSUPP`. | Enable capabilities only after their hardware contracts pass. |
| Validate isolation and recovery across all eight DSPs | Empty-transport reset isolation and exact recovery pass for every DSP. | Repeat with a proven program and per-engine buffer fault injection, then complete the 56-case matrix. |

## Material progress in the latest pass

- Captured four repeatable 770-dword `0x80030302` responses. Their request
  tokens match the preceding commands, their 768-dword bodies have one stable
  hash, and every body entry is one of four high-byte state values.
- Captured exact `0x80070004` intermediate successes for official form-zero
  resources `0x120` and `0xd0`. Resource IDs and envelope command words match
  in both responses, validating the live `Bill` path and two pool offsets.
- Instantiated RealVerb-Pro through REAPER. The official host later disabled it
  with `-38`, exactly the statically recovered all-zero-response result. No
  complete program execution is claimed.
- Added a metadata-only response decoder, exact-boundary watcher hardening,
  synthetic classifiers, and a sanitized Experiment 025 result.
- Ran an isolated Windows 11 reference guest with Secure Boot, TPM, the exact
  official UAD 11.0.1 media, and the WHQL-attested OCTO driver.
- Captured consumption of the exact HBUT extended header, 624 full pages, the
  final 2,192-byte descriptor, and the posted response descriptor.
- Performed RTC-backed cold recovery, then captured all 16 official ring bases,
  all-eight DMA enable, two DSP0 command pairs, and two response-descriptor
  consumption boundaries.
- Proved that both exact-boundary response targets stayed zero and corrected
  the response read-index interpretation accordingly.
- Repeated connect plus query 026 after the update and cold boot. The command
  consumed, the response canary remained intact, and exact recovery passed.
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

A speculative arbitrary `Bill` submission still lacks a proven executable
target, entry point, and memory-safety contract. Experiment 025 supplied two
lawful official resource successes but the complete load ended in `-38`. The
official HBUT experiment has resolved the host transport but not the payload or
persistence mechanism. Repeating the update or substituting an invented
resource would produce an ambiguous result while adding card or host risk.

This is an evidence gate, not an assertion that the remaining work is
impossible. Operation `0x6f` and another firmware update are no longer on the
critical path. The next highest-value observation is the first all-zero target
in the now-captured lawful multi-resource sequence, followed by the DSP-side
consumer of an accepted form-zero `Bill` core.
