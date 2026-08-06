# Reverse-engineering completion status

This project now reconstructs the UAD-2 OCTO host transport through an
authenticated official program-buffer transaction, but it is not a full
arbitrary-code reverse engineering. The remaining boundary is narrow and
important: the DSP-side decoder for the opaque authenticated `Bill` and HBUT
payloads has not been recovered.

## Requested outcomes

| Requested outcome | Current result | What is still required |
|---|---|---|
| Receive and decode a valid firmware response | Runtime responses are decoded. Linux received `0x800c0005`, the authorization response and all host-visible states are understood, all 13 resources received correlated `0x80070004`, and Process returned `0x80020044` with request, channel, and marker `0xf001000e`. The separate HBUT update target stayed zero. | Decode HBUT completion only if cold framework boot remains a goal. The runtime response blocker is closed. |
| Reproduce the complete shared 4 MiB DMA transport | Resolved as inapplicable on this OCTO. Static capability logic and the BAR snapshot show that the optional audio-extension tables are absent. | Use another supported endpoint to study that optional path. It must not be fabricated on this card. |
| Identify exact SHARC DSP models and memory maps | All eight packages are optically confirmed as `ADSP-21469 KBCZ-00`; the family map and four live resource pools are documented. Block-0 DM32/PM48 alias arithmetic identifies two adjacent private allocations as exact 100-instruction PM48 spans. | The speed ordering suffix is not visible. The two code-shaped spans require a debug trace before they can be called executable. Inner stack, interrupt, circular-buffer, and overlay reservations remain hidden. |
| Reverse engineer DSP boot and reset control | Ring startup, all-eight DMA enable, ready polling, isolated reset pulses, device hard reset, firmware-aware `+0x1a8` fork, official update, RTC cold cycle, and shutdown are recovered. | DSP ROM boot and the first framework instruction are not decoded. This is no longer a blocker for resident-framework jobs. |
| Understand firmware and plug-in container formats | FBUT/GBUT/HBUT outer wrappers, all 47 artifacts, full official HBUT transport, `Bill` header, host transform, envelope, allocator, lifecycle, native `0x0af8` allocation record, memory specification, Process buffers, readback, and completions are recovered. Standard clear SHARC LDR framing is absent from the exact RealVerb wire bodies. | Decode the inner HBUT and `Bill` bodies. |
| Determine signing, authentication, relocation, and loading rules | Whole-body device authentication is proven dynamically. The outer mapped-resource relocation layer is proven. The official generic ADSP-21469 ELF, standard loader blocks, Flat-V6 dynamic-module header, section widths, exported symbols, and SHARC relocation arithmetic are now recovered. | Prove whether the UAD post-decode image is Flat-V6; recover the Bill authentication or encryption path, UAD entry adapter, and framework reservations. |
| Identify the first failing object in RealVerb's sequence | Closed. Every one of the 13 Bill objects succeeds. The earlier host `-38` occurs after resource loading, so there is no failing Bill object in the captured sequence. | The wet-effect activation state remains unobserved; default Process output is a dry roundtrip. |
| Load a harmless DSP0 program | The exact authenticated official RealVerb allocation completes and performs a bounded 64-sample stereo roundtrip on DSP0. A bounded user-authored affine source, linker layout, Flat-V6 build pipeline, and strict image validator are implemented. | Activate the official compiler, recover the UAD accepted-object and call adapters, then execute matched affine vectors. No custom UAD instruction has run yet. |
| Implement host-to-DSP buffers and completion handling | Achieved for the authorized workload. Two 66-dword inputs, two 68-dword outputs, request IDs, channel IDs, Process marker, bounded writes, and completion retrieval are implemented. | Asynchronous queues, cancellation, and custom-program buffer contracts are future work. |
| Build reusable Linux driver and userspace API | ABI version 2 implements buffers, bounded `mmap`, authorized program load, synchronous submit, wait, recovery, and target selection. Capability bitmap is `0x3f`. | Broaden only after a new program format can be validated. The current loader intentionally accepts only the exact authorized RealVerb bundle. |
| Validate isolation and recovery across all eight DSPs | Normal authorized program-buffer jobs pass independently on DSP0 through DSP7. Non-target ring indices stay unchanged, writes are bounded, and reset plus IOMMU recovery pass. A mutated bundle is rejected and an unchanged retry succeeds. | Hostile custom-program fault isolation cannot be tested without an arbitrary-code loader. Concurrent multi-program scheduling is not implemented. |

## Latest evidence

- Hash-locked static analysis proves that `CPluginInstance` stores the mapped
  first private resource at object offset `0x0bd8` and places it in word three
  of the main Process command.
- The correct command is
  `000b0004 00400000 request_id 0009d00a`. The prior `0x000e0000` hypothesis
  addressed the first public Bill allocation. Zero and impulse controls at
  that wrong address had identical output hashes, so Experiments 040 through
  042 are explicitly superseded.
- Experiment 044 ran eight 64-sample stereo ticks. Tick zero returned
  `0x3f000000` and `0xbf000000` exactly. Ticks one through seven returned zero.
  Every response carried header `0x80020044`, the expected request counter,
  the channel number, and marker `0xf001000e`.
- Experiment 045 repeated the corrected job for every target DSP. All 13
  resources were accepted per trial, all outputs matched, all non-target ring
  indices remained unchanged, and cleanup passed.
- ABI version 2 exposes six validated capabilities without raw MMIO, physical
  addresses, unrestricted command submission, or arbitrary images.
- Experiment 046 loaded the Secure Boot signed module and completed one
  authorized job on each DSP. Afterward all eight DSPs were ready, DMA was
  disabled, and every command and response index was zero.
- Experiment 047 changed one byte in the private bundle. The device rejected
  it, the API returned `EKEYREJECTED`, and an unchanged bundle immediately
  loaded and completed after recovery.
- Experiments 048 and 049 recovered the exact native allocation record and a
  valid Process-coupled readback response. The complete 430-dword private
  Process object contains only 32 host-patched allocation addresses and zeros.
- Experiment 050 showed that the same operation does not return the first
  public Bill allocation. The command is consumed while its response and
  canary remain untouched.
- The exact RealVerb DLL contains 13 adjacent generation pairs. Their core
  equal-byte fraction averages 0.003178 and pairwise XOR entropy averages
  7.588078 bits per byte. No standard decompression, aligned-block, or tested
  digest hypothesis explains the core.
- A standard SHARC loader scan over all 26 transmitted RealVerb bodies found
  zero plausible block headers at 45,398 offsets in either word endianness.
- Experiment 052 installed the official legacy SHARC toolchain and validated a
  clear ADSP-21469 DXE plus its standard `ZERO_L48`, `INIT_L48`, and
  `FINAL_INIT` stream. It also recovered the official Flat-V6 `bFLT` revision-6
  section, relocation, and exported-entry format used by ADSP-21469 dynamic
  modules.
- Experiment 053 performed an RTC power-off and official-driver restart. The
  resulting trace republished all command and response rings and enabled DMA
  for all eight DSPs. It did not expose firmware-transition magic or ordinary
  hard-reset writes, so it proves fresh host transport publication but not a
  captured firmware upload.
- Experiment 054 completed the previously blocked private-selector retry.
  Allocation index 1 at `0x9cf74` returned a valid 150-dword response containing
  only zeros. Its numerical PM48 code shape is therefore not evidence of a
  clear executable under the recovered readback path.

## What “full” would mean

For this repository, full arbitrary-code reverse engineering requires a
reproducible path from a user-controlled ADSP-21469 program to a device-accepted
object. That path must document:

1. inner authenticated cleartext or a lawful object-creation mechanism;
2. code and data segment records and address units;
3. DSP-side relocation records and arithmetic;
4. entry point, stack, interrupt, and calling conventions;
5. framework-reserved memory ranges;
6. bounded failure handling for a malformed or hung custom program; and
7. normal and fault isolation with simultaneous programs.

The repository now has a processor-valid affine source and a strict clear-image
toolchain, but it still cannot derive a UAD-accepted object from high-entropy
ciphertext, and the card rejects changes to official objects. No signing or
licensing bypass is implemented or claimed. The practical result is a reusable
Linux interface for captured authorized workloads plus an offline custom SHARC
build path, not yet a general-purpose UAD execution path.

Detailed evidence is in
[`experiment-039-047-process-api-isolation.md`](experiment-039-047-process-api-isolation.md),
[`experiment-048-050-runtime-readback.md`](experiment-048-050-runtime-readback.md),
[`experiment-053-054-framework-refresh-selector1.md`](experiment-053-054-framework-refresh-selector1.md),
[`experiment-052-custom-kernel-toolchain.md`](experiment-052-custom-kernel-toolchain.md),
[`driver-api.md`](driver-api.md), and
[`bill-resource-analysis.md`](bill-resource-analysis.md).
