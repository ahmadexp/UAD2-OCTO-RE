# Program execution, API, and isolation gates

This is the implementation contract for the remaining general-purpose DSP
work. It prevents a transport success from being misreported as executable
control.

## Minimal harmless DSP0 program

A first program is acceptable only when all of these fields are proven for the
exact OCTO target:

| Required field | Current state |
|---|---|
| executable machine family and package | ADSP-21469 family is strong; package suffix unconfirmed |
| code and data address units | data-sheet map known; loader interpretation unknown |
| segment records and alignment | unknown |
| relocation records and arithmetic | unknown |
| entry point and call ABI | unknown |
| runtime-reserved ranges | four outer pools observed; allocations inside an opaque program unknown |
| stack, interrupt, and circular-buffer reservations | unknown |
| authentication or integrity rule | unknown |
| program-load command and success completion | related Apollo evidence only, conflicting constants |
| unload and failure recovery | empty-engine reset proven, loaded-program recovery unproven |

The first payload should do only one bounded action: increment a counter in a
dedicated IOMMU buffer and return or wait. It must not touch audio I/O, flash,
FPGA configuration, other DSPs, or unrestricted host addresses.

## Load acceptance criteria

A load is successful only if all of the following are captured:

1. the loader consumes the exact program object;
2. a documented success response is received;
3. the program writes only inside its assigned IOVA buffer;
4. a monotonically increasing heartbeat or explicit completion is observed;
5. timeout stops DSP0 without changing DSP1 through DSP7 status;
6. DSP0 reset returns its rings and DMA state to the baseline;
7. IOMMU teardown reports no mapping or fault leakage;
8. cold baseline is independently recaptured after unload.

An advanced ring index without a response and bounded output is not a program
load.

## Linux API progression

The current kernel and userspace interfaces intentionally expose only proven
transport and status operations. Program, buffer, submit, and wait entry points
return `-EOPNOTSUPP`.

Future interface milestones must be enabled one at a time. The names below are
evidence milestones, not additional version-1 UAPI constants:

| Capability | Evidence gate |
|---|---|
| `RUNTIME_RESPONSE` | one decoded benign official response |
| `PROGRAM_VALIDATE` | offline parser rejects malformed segments and relocations |
| `PROGRAM_LOAD_DSP0` | exact load ABI and success completion |
| `DMA_BUFFER` | program-visible IOVA width, alignment, lifetime, and direction |
| `JOB_SUBMIT` | request ID and ownership semantics |
| `JOB_WAIT` | completion record, timeout, and cancellation semantics |
| `MULTI_DSP` | repeatable DSP0 recovery plus target selection in the proven ABI |

No ioctl may accept arbitrary physical addresses or unrestricted MMIO offsets.
Every mapping remains owned by the kernel, bounded by the IOMMU, associated
with one open file, and revoked on close, timeout, reset, or process death.

## Eight-DSP isolation matrix

Program-level isolation requires more than the completed empty-reset test.
For target DSP `i`, each test records status for all eight engines before,
during, and after the fault:

| Fault case | Expected target behavior | Expected non-target behavior |
|---|---|---|
| normal heartbeat | completes | unchanged |
| program timeout | DSP `i` reset and job fails | counters and ring indices continue |
| malformed program rejected offline | no hardware write | unchanged |
| response timeout | buffers revoked, DSP `i` reset | unchanged |
| process exit during job | ownership revoked, DSP `i` recovered | unchanged |
| IOMMU write outside buffer | fault captured, DSP `i` disabled | no memory or status change |
| repeated reset | deterministic baseline | unchanged |

The full campaign is 8 targets times 7 cases, followed by concurrent pairs
and then all-eight saturation. Advancement requires zero cross-DSP register,
ring, buffer, or interrupt changes outside the documented shared masks.

## Public prior-work audit

Open Apollo provides valuable related-endpoint hypotheses, but its source
currently contains two incompatible firmware loaders:

- an older function claims one contiguous descriptor and includes a response
  value in the transmitted data;
- a later `ua_dsp_send_block` implementation correctly treats the response
  class as validation-only and uses a response descriptor, extended header,
  and per-page payload descriptors.

The exact official PCIe assembly supports the later framing and contradicts
the older one. The later public code changes payload chunks to 1 KiB after an
Apollo-specific stall and caps blocks at 256 KiB. Those choices are not the
official OCTO loader and cannot carry its 2.5 MiB HBUT unchanged.

The same source contains captured Apollo x4 `Bill` programs and conflicting
program-command commentary and constants. Those bytes and constants are not
target-compatible proof for an OCTO. They must not be copied into a DSP0 test.

The current public header has now been audited at a fixed commit. Its five
cores exactly match five official UAD 11.0.1 cabinet cores; only the high byte
of the outer resource ID differs. This establishes common resource content and
supports the public semantic labels, but the accompanying module entry points,
code offsets, and SRAM addresses still come from Apollo x4 runtime captures.
They cannot define OCTO reservations or an OCTO heartbeat ABI.

The stepbrobd/uad2 project contributes useful macOS transport observations but
does not establish general-purpose OCTO program execution. This repository
therefore keeps all related-project conclusions labeled as hypotheses until an
exact official binary or the physical OCTO confirms them.
