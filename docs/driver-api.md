# Experimental Linux driver and userspace API

The repository contains a Linux PCI driver, a C library, and a command-line
client for the exact OCTO endpoint `1a00:0002/1a00:0005`. ABI version 2 reaches
an authenticated official program-buffer transaction on every DSP. It is a
research API for one captured authorized workload, not a generic SHARC loader.

## Hardware and privilege boundary

The `uad2_compute` kernel module:

- refuses every PCI profile except `1a00:0002/1a00:0005` with a 64 KiB BAR;
- verifies FPGA revision `0xa012dc0d`, capability word `0x00300811`, cold DMA
  state, empty ring registers, and all eight ready words before binding;
- allocates 64 coherent ring pages, four per command and response ring;
- reproduces all-eight startup, stop, and isolated per-DSP reset;
- requires `CAP_SYS_RAWIO` for every operation that changes transport or DMA;
- exposes one device node with mode `0600` and exclusive open; and
- stops transport and frees program and buffer state on close or removal, and
  revokes outstanding coherent-buffer mappings before PCI removal.

There is no BAR mapping, physical-address field, raw descriptor interface,
unrestricted command ioctl, or arbitrary program-image operation.

## ABI version 2 capabilities

`UAD2_COMPUTE_IOC_GET_INFO` reports ABI version 2 and capability bitmap `0x3f`:

- `UAD2_CAP_RING_TRANSPORT`
- `UAD2_CAP_PER_DSP_RESET`
- `UAD2_CAP_PROGRAM_LOAD`
- `UAD2_CAP_DMA_BUFFERS`
- `UAD2_CAP_JOB_COMPLETION`
- `UAD2_CAP_PROGRAM_ISOLATION`

`PROGRAM_LOAD` means exact authorized RealVerb bundle loading. It does not mean
arbitrary ADSP-21469 executable loading.

## Buffers

`ALLOC_BUFFER` accepts an input or output direction and at most one 4 KiB host
page. The tested frame is exactly 512 bytes:

```text
2 channels x 64 samples x 4 bytes
```

The kernel returns an opaque buffer ID and `mmap` offset. Userspace never sees
the DMA address. The library maps the page with `MAP_SHARED` and retains the
mapping until `uad2_compute_free_buffer` or close.

All 64-bit UAPI members use explicit eight-byte alignment so the ABI has one
layout for native and compatible userspace. PCI removal revokes every mapping
before coherent storage is released; subsequent calls on an already-open file
return `ENODEV`.

## Authorized program image

The image is 69,632 bytes, or 17 page-sized slots:

```text
slots 0..15   exact authenticated RealVerb transport chunks
slot 16       exact 65-dword private-resource memory specification
padding       zero
```

[`tools/pack_realverb_program.py`](../tools/pack_realverb_program.py) builds
this image from a private Experiment 029 capture. It hash-checks every source
and prints `private_bundle=true do_not_commit=true`. The resulting bundle must
not be added to the repository.

The kernel checks image length, resource IDs, command words, allocation
addresses, Bill magic, body lengths, descriptor splits, memory specification,
and all padding. It then submits each exact resource to the card. A device
rejection becomes `EKEYREJECTED` and triggers stop and cleanup. This design
leaves authentication authority with the card.

## Job contract

`SUBMIT_JOB` accepts a target DSP, one loaded-program handle, one input-buffer
handle, and one output-buffer handle. It builds:

- two 66-dword input objects;
- two 68-dword response objects; and
- main command `0x000b0004 0x00400000 request_id 0x0009d00a`.

The implementation waits for command and response indices, then validates each
response as `0x80020044`, request ID, channel, `0xf001000e`. It also verifies
that all non-target command and response read indices remain unchanged. Output
samples are copied into the caller's output buffer only after validation.

Submission is synchronous. `WAIT_JOB` returns the already-completed job record,
including status, request ID, and marker. The timeout argument is validated but
does not create an asynchronous wait in ABI version 2.

On a protocol, response, timeout, or isolation error, the module frees the
loaded program and stops transport. A later open can start from the cold
precondition.

## Build

On the Linux target, build against the running kernel:

```bash
make -C kernel
make -C lib
```

The userspace artifacts are `lib/libuad2compute.a` and `lib/uad2ctl`.

## Read-only inspection

These operations do not start DMA:

```bash
sudo lib/uad2ctl info
sudo lib/uad2ctl status
```

## Transport and reset

```bash
sudo lib/uad2ctl start
sudo lib/uad2ctl reset 0
sudo lib/uad2ctl stop
```

The version-1 transport regression remains available:

```bash
sudo env \
  UAD2_COMPUTE_MODULE="$PWD/kernel/uad2_compute.ko" \
  UAD2_COMPUTE_CTL="$PWD/lib/uad2ctl" \
  tools/uad2-compute-transport-test.sh
```

## Authorized impulse job

After privately packing the exact bundle:

```bash
python3 tools/pack_realverb_program.py \
  /private/experiment-029-deadline-safe-sequence \
  /private/realverb.bundle

sudo lib/uad2ctl run 0 /private/realverb.bundle
```

The tested job writes positive half-scale to channel zero sample zero and
negative half-scale to channel one sample zero. Success reports the program ID,
job ID, request ID, marker `0xf001000e`, and exact output dwords.

Do not load the module alongside VFIO or a vendor driver. Do not unload it
while a control operation is active.

## Secure Boot

The tested host enforces Secure Boot module signatures. A dedicated local
Machine Owner Key signed the module, its public certificate was enrolled
through MokManager, and the signed module loaded. Signature enforcement was
not disabled or bypassed. The private key remains outside this repository.

Rebuild and sign the module after every code or kernel change. Key generation
and enrollment are host trust operations and are intentionally not automated
by an experiment wrapper.

## Files

- `include/uapi/uad2_compute.h`: fixed-width ABI structures and ioctls
- `kernel/uad2_compute.c`: exact-device driver and guarded operations
- `lib/uad2_compute.h`: public C API
- `lib/uad2_compute.c`: userspace implementation and buffer mappings
- `lib/uad2ctl.c`: state, reset, and authorized impulse client
- `tools/pack_realverb_program.py`: private exact-bundle packer

Hardware evidence and claim boundaries are documented in
[`experiment-039-047-process-api-isolation.md`](experiment-039-047-process-api-isolation.md)
and [`program-execution-gates.md`](program-execution-gates.md).
