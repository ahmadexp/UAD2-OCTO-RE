# Reverse-engineering completion status

This project has a substantial host-transport reconstruction, but it is not a
full reverse engineering of the UAD-2 OCTO. The distinction matters because a
working ring is not the same as a working DSP runtime, and an outer container
parser is not the same as a decoded executable.

## Requested outcomes

| Requested outcome | Current result | What is still required |
|---|---|---|
| Receive and decode a valid firmware response | Achieved for runtime services. Linux received `0x800c0005` from query 026, the four authorization wire values have exact display meanings, and the exact `0x12b` resource received `0x80070004` success. The separate HBUT update completion stayed zero. | Decode HBUT completion only if cold framework boot remains a project goal. |
| Reproduce the complete shared 4 MiB DMA transport | Resolved for this OCTO profile as inapplicable. Static capability logic and a read-only snapshot show that the optional audio-extension object and BAR `0x8000`/`0xa000` tables are absent. | A different supported device is required to execute that optional path. It should not be fabricated on this OCTO. |
| Identify exact SHARC DSP models and memory maps | Achieved at the component level. The high-resolution photograph confirms `ADSP-21469 KBCZ-00` on all eight SHARCs, and the family data-sheet map is transcribed. | Recover the board-specific runtime reservations and loader map; the package marking does not independently prove the `-3` or `-4` speed grade. |
| Reverse engineer DSP boot and reset control | Host reset masks, ready polling, ring startup, all-eight DMA enable, isolated per-DSP reset, ordinary hard reset, and the firmware-aware `+0x1a8` fork are recovered. The official update, RTC cold cycle, post-update startup, and ordinary shutdown reset are traced. The magic fork was not observed or synthesized. DSP ROM boot flow and first framework instruction are not decoded. | Trace a plug-in program load into the DSP-side boot or overlay consumer. |
| Understand firmware and plug-in container formats | Fixed FBUT/GBUT/HBUT wrapper, all 47 artifacts, full official HBUT transport, `Bill` outer header, conditional transform, envelope, allocator, copy limit, and completion parser are recovered. The complete first 13-resource RealVerb pass is accepted from Linux. | Decode the opaque HBUT payload and the authenticated `Bill` cleartext. |
| Determine signing, authentication, relocation, and loading rules | Device-side integrity or authentication over both the 48-byte prefix and 384-byte core is dynamically proven. The fixed `0x0af0` host allocation record, 16-byte runtime memory specs, `0x00150000` mapped-address patch command, and 8-byte readback specs are recovered. Exact host mappings for pre-completion errors `0x0005` and `0x000d` are also recovered. | Identify the inner algorithm and key source, then recover clear segments, the DSP-side relocation and entry rules, and reservations. The recovered memory-spec patch is the outer host runtime relocation layer, not proof of the opaque core format. |
| Load a harmless DSP0 program | All 13 exact resources in the captured RealVerb pass are accepted sequentially, including three two-descriptor objects, but no module entry point or bounded output was activated. | Obtain the official allocation and `Process` metadata, then recover a lawful clear executable and OCTO entry ABI before running a bounded heartbeat. |
| Implement host-to-DSP buffers and completion handling | Ring DMA, valid query response, complete resource-pass completion, and all-eight loader targeting are reconstructed and observed. Public compute calls remain fail-closed. | A program-visible IOVA contract, one bounded buffer exchange, and job completion before enabling UAPI job calls. |
| Build reusable Linux driver and userspace API | Bounded kernel transport, versioned userspace API, identity checks, coherent ring allocation, start, status, reset, and stop exist. Program/buffer/job/wait operations return `-EOPNOTSUPP`. | Enable capabilities only after their hardware contracts pass. |
| Validate isolation and recovery across all eight DSPs | Empty-transport reset isolation passes, and the same authenticated resource is accepted independently by all eight DSPs with every non-target ring index unchanged. | Activate a bounded program, inject per-engine buffer faults, then complete the 56-case matrix. |

## Material progress in the latest pass

- Decoded all authorization wire states and their official display meanings.
  The current table contains 26 authorized, 203 demo-not-started, and 539
  authorization-update-required entries; `0x81` and `0x82` intentionally
  collapse to the same authorized state.
- Bridged the resident official framework to Linux and received query 026's
  exact `0x800c0005` response in 1 ms under a 66-page IOMMU mapping.
- Identified `0x12b` as the first all-zero RealVerb target in the invasive
  sequence capture, then proved with Linux that the unchanged object is valid
  and receives exact `0x80070004` success in 1 ms.
- Flipped one bit at six approved body offsets. Every mutation was rejected;
  the first prefix byte returned `0xf0010005`, while the last prefix byte and
  all sampled core bytes returned `0xf001000d`. Unchanged controls before and
  after were accepted.
- Recovered the fixed public host mappings `0xf0010005 -> -55` and
  `0xf001000d -> -60`. The DSP-side semantic names remain unknown.
- Submitted the exact authenticated resource independently to DSP0 through
  DSP7. Every engine accepted it in 1 ms, all seven non-target index sets
  remained unchanged per trial, all DSPs stayed ready, and recovery passed.
- Recovered the complete host-side plug-in runtime metadata layout: native
  `0x0af0`-byte allocation records, 16-byte memory specs at `0x188`, 8-byte
  readback specs at `0x98c`, and the `0x00150000` mapped-address patch command.
- Recovered resource readback command `0x000c0004` and its exact
  `requested_dwords + 2` response size. A fixed four-dword live read after
  accepted `0x12b` loading was consumed but produced no response in 6 seconds;
  bounded cleanup and VFIO reset passed. Static call order and this negative
  trial identify complete plug-in processing as the next prerequisite.
- Replayed all 13 exact resources from RealVerb's first pass under Linux. Every
  resource returned its exact ID- and command-correlated `0x80070004` response
  in 1 to 5 ms, including `0xf9`, `0xbf`, and `0xd1` across two descriptors.
  A fixed readback after the complete pass remained unanswered, while DMA
  canaries, all seven non-target ring-index sets, ready state, restore, and
  VFIO reset all passed.
- Recovered exact `0x00080004` zero-resource and `0x00030002` pool-zero
  unmap semantics from the public driver. All 13 captured cleanup commands
  were consumed in 1 ms each. A later allocation preflight remained
  fail-closed: the first resource and query 026 were consumed without
  responses, and the proven per-DSP reset path did not restore the runtime
  service. No zero-resource or memory-spec command was submitted.

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

A speculative arbitrary `Bill` submission still lacks a proven clear
executable, entry point, and memory-safety contract. Experiment 029 now shows
that changing either the preserved prefix or encrypted-looking core is rejected
by the device, so an invented heartbeat cannot be placed inside the accepted
container. Experiment 031 also shows that the recovered readback command is not
a standalone decoded-memory oracle after isolated resource acceptance. The
official HBUT experiment resolved the host transport but not the payload or
persistence mechanism.

This is an evidence gate, not an assertion that the remaining work is
impossible. Operation `0x6f`, another firmware update, the former first
all-zero target, and the complete 13-resource pass are no longer on the
critical path. The remaining critical artifact is the official plug-in
allocation and `Process` metadata, a lawful cleartext representation of one
authenticated core, or a DSP-side loader trace that reveals decoded segments
and the entry ABI.

The current live card additionally needs a fresh official plug-in activation
before the allocation preflight can resume. Ready bits, ring reset, VFIO reset,
and per-DSP reset all pass, but query 026 no longer produces a response after
the one-shot complete resource pass.
