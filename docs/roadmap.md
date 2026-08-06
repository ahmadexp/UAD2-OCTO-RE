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
- [x] Capture the exact native allocation record and its readback specification.
- [x] Receive a valid Process-coupled resource-readback response.
- [x] Snapshot the complete private Process object and verify every host patch.
- [x] Rule out clear standard SHARC LDR headers in the RealVerb wire bodies.
- [x] Rule out the official readback operation as a public Bill pool oracle.
- [x] Recover the official clear ADSP-21469 ELF, standard LDR block, Flat-V6
      dynamic-module, exported-symbol, and generic relocation formats.
- [x] Identify two adjacent 150-DM32-word private allocations whose physical
      aliases are two exact 100-instruction PM48 spans.
- [ ] Test one additional known private-resource selector after a fresh
      official activation; the bounded selector probe is implemented, but its
      first attempt stopped at the stale framework prerequisite.
- [ ] Identify the inner authentication or encryption algorithm and key source.
- [ ] Decode clear code and data segments.
- [ ] Decode DSP-side relocation records and arithmetic.
- [ ] Decode entry point, call ABI, and runtime reservations.

The exact RealVerb DLL provides 13 adjacent generation-1/generation-2 pairs.
Their cores have near-random cross-generation differences, no standard
decompression succeeds, and tested digest and repeated-block hypotheses fail.
The exact wire bodies also contain no plausible standard SHARC loader headers
at any byte offset in either word endianness.
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

- [x] Install the official legacy SHARC toolchain and establish hash-locked
      clear ADSP-21469 DXE/LDR references.
- [x] Recover and validate the documented ADI Flat-V6 dynamic-module format,
      section widths, exported entry table, and SHARC relocation records.
- [x] Implement bounded affine source, linker layout, build driver, ABI
      manifest, ELF/DLM/LDR validator, and block-0 DM32/PM48 alias checker.
- [ ] Activate the official CCES evaluation and produce the affine DXE/DLM.
- [ ] Prove that the UAD post-decode representation is Flat-V6, or identify
      the different UAD-specific clear representation.
- [ ] Recover the UAD framework adapter from Process state to an exported
      user function.
- [ ] Create a minimal DSP0 heartbeat that touches one assigned buffer only.
- [ ] Demonstrate a fixed `out[i] = a * in[i] + b` affine kernel.
- [ ] Add one stateful biquad, then a short FIR after affine isolation passes.
- [ ] Add partitioned convolution only after the overlay and memory ABI is known.
- [ ] Inject timeout and bounded IOMMU faults on each target DSP.
- [ ] Run concurrent programs only after single-target fault recovery passes.

## Current blockers

| Goal | Blocking evidence | Required next evidence |
|---|---|---|
| inner authentication | prefix and core mutations are rejected; host parser performs no cryptography | DSP-side decode trace, lawful development artifact, or documented format |
| UAD clear executable | generic ADSP-21469 ELF/LDR/Flat-V6 is now known, but Bill cores are high-entropy and generation-specific | stop DSP0 at the resource consumer and compare its post-decode memory with Flat-V6 |
| UAD DSP-side relocation | all generic Flat-V6 SHARC relocation forms are known, but the UAD consumer has not been observed using them | before-and-after loader memory trace at the two code-shaped private spans |
| custom entry point | the affine export is defined, but Process enters the official private object rather than a user-selected function | captured DSP0 call state and a verified UAD-to-affine adapter stub |
| lawful compiler output | CCES 2.12.1 is installed, but the compiler correctly refuses to run without activation | user activation of the official evaluation license |
| hostile-code isolation | no accepted program can intentionally hang or fault | accepted minimal custom program with bounded fault variants |

## Stop conditions

Stop or redesign if the only path requires bypassing authorization, modifying
persistent board firmware, exposing unrestricted DMA or MMIO, or submitting an
object whose memory behavior cannot be bounded. The project may continue to
support authorized official workloads even if the inner cryptographic format
cannot be lawfully reproduced.
