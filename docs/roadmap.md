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
- [ ] Determine whether executable containers are signed or authenticated.
- [ ] Recover relocation, segment, entry-point, and memory-protection rules.
- [x] Build a bounded offline wrapper parser with synthetic tests.
- [ ] Decode and parse the transformed payload into segments and relocations.

The exact `HBUT` artifact matches the target's FPGA revision, but the updater
warns against power loss. It remains outside the experiment boundary until the
persistent update path is separated from the volatile DSP framework loader.

## Phase 3: first controlled program

- [ ] Build a minimal heartbeat for DSP0.
- [ ] Load it without touching DSP1 through DSP7.
- [ ] Verify a counter or completion record in a dedicated IOMMU buffer.
- [ ] Add timeout and per-DSP recovery before repeating.
- [ ] Demonstrate `out[i] = in[i] + constant` with explicit input and output
      buffers.

## Phase 4: reusable compute interface

- [ ] Linux kernel transport with no arbitrary MMIO or physical-address API.
- [ ] Userspace library for capabilities, buffers, programs, jobs, and waits.
- [ ] Per-DSP scheduling and failure isolation.
- [x] Validate empty-transport reset isolation across all eight DSP engines.
- [ ] Validate program and buffer isolation across all eight DSP cores.
- [ ] Performance measurements against CPU implementations.

## Stop conditions

The project should stop or redesign if executable authentication cannot be
lawfully and safely satisfied, the FPGA exposes DMA outside the IOMMU contract,
reset cannot recover a failed DSP, or the only viable path requires modifying
persistent board firmware.
