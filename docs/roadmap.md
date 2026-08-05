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
- [ ] Receive one benign response from resident firmware.

## Phase 2: loader and executable format

- [x] Identify the ADSP-21469 family and transcribe its data-sheet memory map.
- [ ] Confirm the exact package marking on the tested board.
- [ ] Separate persistent FPGA firmware, DSP framework, and plug-in containers.
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
- [ ] Decode opaque payloads into segments and relocations, if those concepts
      are present in the DSP-side format.

The exact `HBUT` artifact matches the target's FPGA revision. The symbolized
driver maps operation `0x69` to `LoadFirmware`, while `LoadFPGAImage` is the
separate operation `0x6a`. The updater nevertheless requires a restart after a
PCIe firmware update, so this distinction does not prove volatility. Experiment
021 consumed only the extended header and recovered safely; further full-image
submission is paused pending proof of the persistence boundary.

## Current blockers

| Goal | Blocking evidence | Required evidence before implementation |
|---|---|---|
| Valid DSP response | Resident connect and query commands dequeue but never write the response ring | Identify and safely enter the runtime state that implements query services |
| Payload authentication and transform | HBUT and official `Bill` bodies remain opaque; four direct SHA-256 layouts fail across all 87 `Bill` instances and no host-side verifier was found for form-zero objects | Recover the DSP-side consumer or obtain a lawful decoded reference artifact |
| Relocations and runtime reservations | Pool bounds are known, but no decoded segment, entry-point, or relocation record is visible | Decode one target-compatible program resource and correlate its allocations |
| Harmless DSP0 program | No proven OCTO executable format or entry ABI exists | Valid framework response plus a decoded, target-specific minimal program format |
| General-purpose job API | Program handles, completion IDs, and buffer ownership would currently be guesses | One real program load, bounded buffer exchange, and completion response |
| Eight-DSP program isolation | Reset isolation is proven only with empty transports | First prove one recoverable program on DSP0, then repeat with per-engine fault injection |
| Exact DSP package | Board photograph does not resolve the package marking | A sharp, perpendicular macro photograph of one DSP marking |

The transport module remains fail-closed for program, buffer, submit, and wait
operations until these evidence gates are met.

Static work also proves that firmware operation `0x69` selects its own target
method and does not automatically invoke operations `0x67` and `0x68` in the
common dispatcher. Six fields in the official 168-byte system-information
record are assigned, but a live official operation-`0x6f` response is still
missing.

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
