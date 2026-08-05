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
- [ ] Recover the complete device-start sequence.
- [ ] Reproduce four-page command and response ring initialization in official
      order.
- [ ] Receive one benign response from resident firmware.

## Phase 2: loader and executable format

- [ ] Identify the exact SHARC model and memory map.
- [ ] Separate FPGA firmware, DSP framework, and plug-in container formats.
- [ ] Determine whether executable containers are signed or authenticated.
- [ ] Recover relocation, segment, entry-point, and memory-protection rules.
- [ ] Build an offline parser with strict bounds and corpus tests.

Firmware-management command constants have been observed statically, but they
remain outside the current experiment boundary. Knowing a command number is not
enough to make it safe.

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
- [ ] Conformance tests for all eight DSPs.
- [ ] Performance measurements against CPU implementations.

## Stop conditions

The project should stop or redesign if executable authentication cannot be
lawfully and safely satisfied, the FPGA exposes DMA outside the IOMMU contract,
reset cannot recover a failed DSP, or the only viable path requires modifying
persistent board firmware.
