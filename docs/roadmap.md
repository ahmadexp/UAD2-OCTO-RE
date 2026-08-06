# Roadmap to arbitrary general-purpose DSP execution

The host transport and one authorized program-buffer path are complete. The
remaining goal is user control over the ADSP-21469 instruction stream.

## Phase 1: host transport

- [x] Identify the exact PCI endpoint, BAR, FPGA revision, and subsystem.
- [x] Establish IOMMU isolation and VFIO reset.
- [x] Recover the four-page ring descriptors and index roles.
- [x] Reproduce all 16 rings and all-eight DMA startup.
- [x] Recover interrupt-vector compression and callback masks.
- [x] Resolve the shared 4 MiB extension as absent on this OCTO profile.
- [x] Receive and correlate valid runtime responses from Linux.
- [x] Recover and validate per-DSP and whole-device reset paths.

## Phase 2: firmware and resource formats

- [x] Confirm all eight `ADSP-21469 KBCZ-00` packages.
- [x] Separate persistent FBUT/GBUT/HBUT update traffic from ordinary Bill
      resources.
- [x] Inventory all 47 firmware wrappers and reproduce the complete official
      HBUT descriptor chain.
- [x] Recover the Bill outer header, transform, envelope, allocator, lifecycle,
      copy limit, and response parser.
- [x] Prove whole-body device authentication with single-bit differentials.
- [x] Accept the complete 13-resource RealVerb sequence from Linux.
- [x] Recover 33 private allocations and the 65-dword memory specification.
- [x] Recover the first-private-resource Process pointer and buffer ABI.
- [ ] Identify the inner authentication or encryption algorithm and key source.
- [ ] Decode clear code and data segments.
- [ ] Decode DSP-side relocation records and arithmetic.
- [ ] Decode entry point, call ABI, and runtime reservations.

The exact RealVerb DLL provides 13 adjacent generation-1/generation-2 pairs.
Their cores have near-random cross-generation differences, no standard
decompression succeeds, and tested digest and repeated-block hypotheses fail.
The physical device also rejects mutations in both the prefix and core. This
is strong evidence that the remaining format is cryptographically protected,
not a plaintext table awaiting a conventional parser.

## Phase 3: authorized buffer execution

- [x] Load the exact 13-resource official workload.
- [x] Apply the private-resource allocation sequence and memory specification.
- [x] Submit zero and impulse controls on DSP0.
- [x] Observe request-correlated, channel-correlated bounded output.
- [x] Run eight sequential ticks and verify bounded writes.
- [x] Repeat normal execution independently on DSP0 through DSP7.
- [x] Reject a mutated bundle and recover for an unchanged retry.

The captured default state returns input samples unchanged and produces no
post-impulse tail. That is sufficient to prove the buffer and completion path,
but not the RealVerb wet algorithm.

## Phase 4: reusable Linux interface

- [x] Kernel driver with no raw MMIO or physical-address UAPI.
- [x] Exact-device identity and cold-state gates.
- [x] Secure Boot signed module validation.
- [x] Kernel-owned coherent input and output buffers with opaque handles.
- [x] Bounded userspace `mmap`.
- [x] Exact authorized-bundle validation and device authentication.
- [x] Synchronous submit and completed-job retrieval.
- [x] Normal target selection and non-target ring checks across all eight DSPs.
- [ ] Asynchronous queues, cancellation, and concurrent programs.
- [ ] Custom-program scheduling and fault isolation.

ABI version 2 intentionally supports one open file, one authorized program,
and one synchronous job at a time. Broader scheduling should wait for a custom
program format so the API is not overfit further to a proprietary audio
plug-in.

## Phase 5: first user-authored program

- [ ] Obtain a lawful decoded reference or documented development container.
- [ ] Build an offline validator for its segments, relocations, entry point,
      and reserved ranges.
- [ ] Create a minimal DSP0 heartbeat that touches one assigned buffer only.
- [ ] Demonstrate `out[i] = in[i] + constant`.
- [ ] Inject timeout and bounded IOMMU faults on each target DSP.
- [ ] Run concurrent programs only after single-target fault recovery passes.

## Current blockers

| Goal | Blocking evidence | Required next evidence |
|---|---|---|
| inner authentication | prefix and core mutations are rejected; host parser performs no cryptography | DSP-side decode trace, lawful development artifact, or documented format |
| clear executable | inner cores are high-entropy and generation-specific | one authenticated object paired with its clear build product |
| DSP-side relocation | outer mapped-resource patches are known, but no inner records are visible | decoded segment image plus before-and-after loader memory trace |
| custom entry point | Process enters the official private object, not a user-selected address | decoded module metadata or vendor development ABI |
| hostile-code isolation | no accepted program can intentionally hang or fault | accepted minimal custom program with bounded fault variants |

## Stop conditions

Stop or redesign if the only path requires bypassing authorization, modifying
persistent board firmware, exposing unrestricted DMA or MMIO, or submitting an
object whose memory behavior cannot be bounded. The project may continue to
support authorized official workloads even if the inner cryptographic format
cannot be lawfully reproduced.
