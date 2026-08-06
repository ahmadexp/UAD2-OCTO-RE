# Roadmap to general-purpose DSP execution

The goal is not merely to submit an existing audio plug-in. A successful
general-purpose path must load independently controlled code, exchange bounded
buffers, report completion, and recover from failure.

## Phase 1: host transport

- [x] Identify the exact PCI endpoint and BAR profile.
- [x] Establish IOMMU isolation and VFIO reset.
- [x] Confirm ring descriptors and index roles.
- [x] Enable only DSP0 DMA under a bounded mapping.
- [x] Reproduce DSP0 interrupt masks.
- [x] Recover the complete device-start sequence statically.
- [x] Reproduce four-page command and response ring initialization in official
      order with DMA disabled.
- [x] Resolve the shared 4 MiB tables as an optional audio transport that the
      OCTO capability branch does not instantiate.
- [x] Recover the eight-DSP compressed interrupt-vector mapping.
- [x] Receive benign card-written responses through the official runtime.

## Phase 2: loader and executable format

- [x] Identify the ADSP-21469 family and transcribe its data-sheet memory map.
- [x] Confirm `ADSP-21469 KBCZ-00` on all eight packages optically.
- [x] Separate FPGA-image, DSP-framework, and plug-in container paths.
- [x] Identify the fixed 64-byte FBUT/GBUT/HBUT wrapper and exact OCTO artifact.
- [x] Inventory all 47 installer firmware containers, their build words,
      compatibility IDs, declared sizes, entropy, and direct SHA-256 tail tests.
- [ ] Determine whether executable containers are signed or authenticated.
- [ ] Recover relocation, segment, entry-point, and memory-protection rules.
- [x] Build a bounded offline wrapper parser with synthetic tests.
- [x] Recover the `Bill` program-resource outer header and deterministic host
      tail transform, including the payload-form branch.
- [x] Recover its two-dword transmit envelope and low/high free-list allocator.
- [x] Recover 4 KiB copy limits, bounded completion waits, both accepted
      response forms, and the exact final status mapping.
- [x] Identify absolute pool bases, bounds, and reserved ranges for all eight
      DSPs with a read-only VFIO snapshot.
- [x] Observe two official form-zero resource allocations and exact
      intermediate-success responses on DSP0.
- [ ] Decode opaque payloads into segments and relocations, if those concepts
      are present in the DSP-side format.

The exact `HBUT` artifact matches the target's FPGA revision. The symbolized
driver maps operation `0x69` to `LoadFirmware`, while `LoadFPGAImage` is the
separate operation `0x6a`. The updater nevertheless requires a restart after a
PCIe firmware update. Experiment 023 used the exact official path: all 625
payload descriptors and the response descriptor were consumed, the updater
requested restart, and RTC cold recovery succeeded. The completion page and
the later exact-boundary runtime response targets remained zero. Experiment
024 showed that the update alone does not enable query 026 after a cold boot.
Experiment 025 then activated the official plug-in path. It produced four
repeatable nonzero authorization-table candidates and exact `Bill` successes
for resources `0x120` and `0xd0`. RealVerb-Pro instantiated but was disabled on
the recovered `-38` all-zero-response path, so a complete program is still not
available.

## Current blockers

| Goal | Blocking evidence | Required evidence before implementation |
|---|---|---|
| Valid response semantics | Valid table and `Bill` responses are captured, but four table states are unnamed and the complete load later reaches an all-zero response | Isolate the first failing resource response and correlate it with the host object |
| Payload authentication and transform | Form-zero byte-for-byte transfer is dynamically accepted twice, but HBUT and `Bill` inner bodies remain opaque. No host-side verifier was found | Recover the DSP-side consumer or obtain a lawful decoded reference artifact |
| Relocations and runtime reservations | Pool bounds and live offsets `0xe023a` and `0xe02fa` are known, but no decoded segment, entry-point, or relocation record is visible | Decode one accepted target-compatible resource and correlate its inner accesses |
| Harmless DSP0 program | An official multi-resource attempt instantiated RealVerb but ended in `-38`; no proven entry ABI or output exists | Resolve the failing response, then derive a target-specific minimal program format |
| General-purpose job API | Resource completions are real, but program handles and buffer ownership would still be guesses | One complete program load, bounded buffer exchange, and completion response |
| Eight-DSP program isolation | Reset isolation is proven only with empty transports | First prove one recoverable program on DSP0, then repeat with per-engine fault injection |

The transport module remains fail-closed for program, buffer, submit, and wait
operations until these evidence gates are met.

Static work also proves that firmware operation `0x69` selects its own target
method and does not automatically invoke operations `0x67` and `0x68` in the
common dispatcher. Both recovered updater callers also dispatch the selected
`FBUT`, `GBUT`, or `HBUT` directly without an automatic pre-operation or
post-operation. The complete operation-`0x6f` path is recovered: it builds
the system-information record from host state and BAR MMIO and never uses the
DSP command ring. A live `0x6f` call is therefore not a remaining response
milestone.

Resource-manager properties 6, 7, and 8 are likewise host-side BAR or cached
state reads. Their recovery explains the eleven-word pool capture but does not
provide a runtime command or response service.

A distinct kernel-lifecycle transition is known. Ordinary hard reset
pulses BAR `+0x221c`; after the firmware-load flag is set, hard reset writes
`0x0be0deaf` to DSP0 `+0x1a8` instead. This cross-platform result narrows the
boot state machine. The official update trace did not emit the magic and the
post-update shutdown emitted the ordinary reset pulse, so the magic remains an
unexecuted branch rather than a missing required step.

Detailed response-state evidence is in
[`runtime-response-state.md`](runtime-response-state.md). The first-program,
API capability, and 56-case isolation acceptance criteria are in
[`program-execution-gates.md`](program-execution-gates.md).

## Phase 3: first controlled program

- [ ] Build a minimal heartbeat for DSP0.
- [ ] Load it without touching DSP1 through DSP7.
- [ ] Verify a counter or completion record in a dedicated IOMMU buffer.
- [ ] Add timeout and per-DSP recovery before repeating.
- [ ] Demonstrate `out[i] = in[i] + constant` with explicit input and output
      buffers.

## Phase 4: reusable compute interface

- [x] Linux kernel transport with no arbitrary MMIO or physical-address API.
- [x] Validate the signed kernel transport on the exact OCTO under Secure Boot.
- [x] Userspace library with versioned capabilities; buffer, program, job, and
      wait calls are present but explicitly return `-EOPNOTSUPP`.
- [ ] Per-DSP scheduling and failure isolation.
- [x] Validate empty-transport reset isolation across all eight DSP engines.
- [ ] Validate program and buffer isolation across all eight DSP cores.
- [ ] Performance measurements against CPU implementations.

## Stop conditions

The project should stop or redesign if executable authentication cannot be
lawfully and safely satisfied, the FPGA exposes DMA outside the IOMMU contract,
reset cannot recover a failed DSP, or the only viable path requires modifying
persistent board firmware.
