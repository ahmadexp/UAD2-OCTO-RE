# Program execution, API, and isolation gates

This file separates the achieved authorized-workload path from the unresolved
arbitrary-code path. A valid official buffer transaction is meaningful, but it
does not reveal the encrypted or authenticated executable format.

## Authorized program-buffer execution

The following fields are now proven on the exact OCTO target:

| Field | Proven result |
|---|---|
| machine family | eight `ADSP-21469 KBCZ-00` packages |
| public resources | 13 exact generation-2 Bill objects, accepted in order |
| private allocations | 33 `0x00080004` zero commands |
| relocation layer | 65-dword `0x00150041` memory specification using mapped resource plus low-24-bit offset |
| Process object | mapped first private resource at `0x0009d00a` |
| main command | `0x000b0004`, flags `0x00400000`, request ID, Process object |
| input | two 66-dword channel objects, 64 samples at word two |
| output | two 68-dword channel objects, 64 samples at word four |
| completion | `0x80020044`, request ID, channel, marker `0xf001000e` |
| bounded output | exact opposed half-scale impulse roundtrip; zero control and seven later ticks remain zero |
| recovery | explicit stop, VFIO reset, and IOMMU unmap pass |
| target isolation | normal job passes independently on DSP0 through DSP7 with non-target ring indices unchanged |

The word-three address is important. `0x000e0000` is the first public Bill
allocation and produced input-independent garbage. Hash-locked static analysis
and live controls establish `0x0009d00a` as the first private resource and the
valid Process object.

## What the output proves

The corrected job satisfies the bounded-execution criteria for the captured
official workload:

1. every public resource receives a correlated success response;
2. allocation and memory-spec commands are consumed;
3. request and channel identifiers correlate with each output;
4. sample data depend on the supplied input;
5. writes stay inside the declared output prefix;
6. non-target rings remain unchanged;
7. the selected DSP and whole card recover; and
8. the IOMMU mapping is removed completely.

No reverb tail appears in the eight-tick stream. The result may be a default,
bypassed, or not-yet-configured RealVerb state. It proves the framework buffer
path, not a wet-effect claim and not a user-authored instruction stream.

## Linux API version 2

The UAPI advertises these hardware-tested capabilities:

| Capability | Contract |
|---|---|
| `UAD2_CAP_RING_TRANSPORT` | all 16 four-page rings and all-eight DMA startup |
| `UAD2_CAP_PER_DSP_RESET` | isolated reset pulse while transport is active |
| `UAD2_CAP_PROGRAM_LOAD` | exact authorized RealVerb bundle only |
| `UAD2_CAP_DMA_BUFFERS` | kernel-owned coherent buffers, opaque handles, bounded `mmap` |
| `UAD2_CAP_JOB_COMPLETION` | synchronous submit and retrieval of the completed request record |
| `UAD2_CAP_PROGRAM_ISOLATION` | non-target ring-index validation and all-eight normal trials |

The program image is exactly 17 pages. Slots zero through fifteen contain the
16 authenticated resource transport chunks. Slot sixteen contains the exact
65-dword memory specification and zero padding. The packer validates the SHA-256
of every private source. The kernel validates outer structure and padding, then
the card remains the authority for inner authentication.

One open file owns the device. The module supports one loaded program and one
synchronous job at a time. `WAIT_JOB` returns the most recent completed record;
there is no asynchronous queue in ABI version 2. Close and removal stop
transport and release owned mappings.

No ioctl accepts arbitrary physical addresses, BAR offsets, raw descriptors,
unrestricted command words, or arbitrary program images.

## Rejection and recovery gate

Experiment 047 changed one byte at bundle offset 100. The device rejected the
resource, the loader returned `EKEYREJECTED`, and recovery restored the cold
state. An unchanged bundle then loaded and completed the exact impulse job.
This proves that the kernel's structural allowlist cannot substitute for
device authentication and that rejection does not strand the runtime.

## Custom harmless program gate

A user-authored heartbeat or affine transform remains blocked on these exact
unknowns:

| Required field | Current state |
|---|---|
| inner clear executable | unknown; core is high-entropy and mutation-protected |
| authentication or encryption algorithm | unknown |
| key source or lawful object creator | unknown |
| code and data segment records | unknown |
| DSP-side relocations | unknown; the outer host memory-spec layer is known |
| entry point and call ABI | unknown |
| stack, interrupt, circular-buffer, and overlay reservations | unknown |
| custom-program timeout behavior | untestable without an accepted custom object |

The first custom payload, once these rules are known, should increment a
counter in one dedicated buffer and return. It must not access audio I/O,
flash, FPGA configuration, other DSPs, or unrestricted host addresses.

## Isolation status

Normal authorized execution has passed on all eight targets. The following
matrix distinguishes achieved tests from tests that require custom code:

| Case | Status | Evidence |
|---|---|---|
| normal authorized job on each DSP | passed | exact output, completion, non-target rings unchanged |
| mutated authenticated object | passed | device rejection, cold recovery, unchanged retry success |
| repeated start, run, stop | passed sequentially | ABI-v2 all-eight campaign |
| hung custom instruction stream | blocked | no arbitrary loader |
| DSP-originated out-of-range IOMMU write | blocked | no controlled faulting program |
| concurrent independent programs | not implemented | driver intentionally owns one program at a time |
| asynchronous cancellation | not implemented | submit is synchronous |

The achieved result supports safe research with the exact authorized workload.
It does not justify advertising hostile-code containment or a multi-tenant DSP
scheduler.
