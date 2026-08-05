# Experiment 019: kernel transport and userspace API

Status: executed successfully on 2026-08-05 after owner-approved Machine Owner
Key enrollment, followed by independent read-only recovery verification.

## Objective

Confirm that the reusable `uad2_compute` Linux module can bind the exact OCTO
profile, reproduce the validated 64-page transport startup, reset every DSP
engine independently, return to the cold state, unload, and leave the endpoint
unbound.

## Preconditions

- PCI endpoint `0000:03:00.0` is `1a00:0002/1a00:0005`.
- BAR0 is 64 KiB.
- The endpoint is unbound and alone in its IOMMU group.
- DMA control is `0x0001fe00`, interrupt enable is zero, all ordinary ring
  words are zero, and every DSP ready bit is set.
- The module is built for the running kernel.

The module repeats the cold-state checks during probe and refuses to bind if
any check fails.

## Bounded resources and writes

The driver allocates exactly 64 coherent 4 KiB pages. It publishes four pages
for each of 16 rings. No userspace physical address or BAR mapping is exposed.

Startup writes only the previously validated registers and ring fields:

- global notification, interrupt, and DMA controls;
- four 64-bit page descriptors plus the two host-owned indices in each ring;
- incremental DSP DMA-enable shadows.

Each per-DSP reset clears one enable bit, pulses its corresponding reset bit,
checks readiness, and restores the prior enable shadow. Stop disables
interrupts, resets DMA, clears every published descriptor and host-owned index,
and restores `0x0001fe00`.

## Acceptance

- Driver binds and reports only ring-transport and per-DSP-reset capabilities.
- Every DSP is ready and DMA enabled after startup.
- All eight reset calls preserve readiness and restore `0x000001ff`.
- Stop reports all DSP DMA-enable bits clear and zero ring indices.
- Module removal leaves the endpoint unbound.
- A separate read-only probe confirms the exact cold state.

## Abort and cleanup

Any identity, IOMMU, module, state, startup, status, or reset failure aborts the
test. A shell trap requests transport stop and module removal. The module's
remove path independently disables DMA and clears ring descriptors if the
transport remains started.

## Explicit exclusions

This experiment submits no command, firmware, `Bill` resource, executable,
host data buffer, or interrupt request. It does not test DSP program isolation.

## Initial signature gate

The module and userspace library compiled on the target against Linux
`7.0.0-29-generic`; the userspace build passed `-Wall -Wextra -Werror`. The
kernel reported `Key was rejected by service` when `insmod` attempted to load
the unsigned module. `mokutil` confirmed that Secure Boot is enabled.

Rejection happened before `uad2_probe`, so the test performed no hardware
write, did not allocate a device DMA mapping, and did not bind the endpoint.
A subsequent independent read-only VFIO probe observed DMA control
`0x0001fe00`, all 176 sampled ring words zero, and all eight DSPs ready. The
endpoint was left unbound.

No signature-enforcement bypass was attempted. A dedicated local key was
created outside the repository, its public certificate was enrolled through
MokManager, and the module was rebuilt and signed for the running kernel.

## Executed result

The signed module loaded and created `/dev/uad2_compute0`. Its identity ioctl
reported ABI version one, the exact PCI and subsystem IDs, FPGA revision
`0xa012dc0d`, extended capabilities `0x00300811`, eight DSPs, and capability
bitmap `0x3`. Only ring transport and per-DSP reset were advertised.

Transport startup published all 64 coherent ring pages. All eight DSPs then
reported ready, DMA enabled, and command/response indices `0/0`. Reset ioctls
for DSP0 through DSP7 each observed DMA control `0x000001ff` before and after
the isolated pulse, with readiness preserved.

Stop cleared every DSP DMA-enable bit and left all indices at `0/0`. Module
removal left the endpoint unbound. The module submitted no command or payload.

An independent read-only VFIO postflight then observed zero DMA mappings, zero
MMIO writes by the verifier, DMA control `0x0001fe00`, all 176 sampled ring
words zero, and all eight DSPs ready. Every acceptance criterion for this
empty-ring transport experiment passed.

This proves the reusable kernel transport and reset isolation. It does not
prove program loading, program-level memory isolation, or job completion.
