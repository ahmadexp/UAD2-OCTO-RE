# Experimental Linux transport and userspace API

The repository now contains a Linux PCI driver, a small C library, and a
command-line client for the exact OCTO endpoint `1a00:0002/1a00:0005`. This is
a transport milestone, not yet a program-execution stack.

## Implemented boundary

The `uad2_compute` kernel module:

- refuses every PCI profile except `1a00:0002/1a00:0005` with a 64 KiB BAR;
- checks the observed FPGA revision, capability word, cold DMA state, empty
  rings, and all eight ready words before binding;
- allocates 64 coherent 4 KiB pages, four pages for each command and response
  ring on each DSP;
- reproduces the validated all-eight empty-ring start and stop sequences;
- reports ready, DMA-enable, and command/response index state per DSP;
- exposes the validated reset pulse for one DSP at a time only while the
  transport is started; and
- requires `CAP_SYS_RAWIO` for start, stop, and reset.

It does not expose BAR mappings, raw register writes, DMA addresses, or an
arbitrary command submission ioctl. Device removal stops DMA and clears the
ring descriptors before freeing coherent memory.

## Capability contract

`UAD2_COMPUTE_IOC_GET_INFO` returns a versioned capability bitmap. Version 1
sets only:

- `UAD2_CAP_RING_TRANSPORT`
- `UAD2_CAP_PER_DSP_RESET`

The following bits remain clear because the corresponding behavior has not
been proven:

- `UAD2_CAP_PROGRAM_LOAD`
- `UAD2_CAP_DMA_BUFFERS`
- `UAD2_CAP_JOB_COMPLETION`
- `UAD2_CAP_PROGRAM_ISOLATION`

The userspace library includes future-facing buffer, load, submit, and wait
entry points, but each returns `-EOPNOTSUPP`. This makes unsupported operations
explicit and prevents applications from mistaking transport startup for
general-purpose execution.

## Build

On the Linux host:

```bash
make -C kernel
make -C lib
```

The module must be built against the running kernel. The userspace artifacts
are `lib/libuad2compute.a` and `lib/uad2ctl`.

## Read-only state

After binding the module, these operations do not start DMA:

```bash
sudo lib/uad2ctl info
sudo lib/uad2ctl status
```

The device node is created with mode `0600`. The output reports all capability
bits and the raw state for each DSP.

## Privileged transport control

On an IOMMU-isolated endpoint in the documented cold state:

```bash
sudo lib/uad2ctl start
sudo lib/uad2ctl reset 0
sudo lib/uad2ctl stop
```

The reproducible target-side test performs all eight reset operations and
always stops the transport from its cleanup trap:

```bash
sudo env \
  UAD2_COMPUTE_MODULE="$PWD/kernel/uad2_compute.ko" \
  UAD2_COMPUTE_CTL="$PWD/lib/uad2ctl" \
  tools/uad2-compute-transport-test.sh
```

Do not unload the module while a control operation is running. Do not use this
module alongside VFIO or a vendor driver.

## Secure Boot

The tested host has Secure Boot enabled and rejected the unsigned research
module before its probe ran. A dedicated local Machine Owner Key was then
created, the module was signed with the running kernel's `scripts/sign-file`,
the public certificate was enrolled through MokManager, and the signed module
loaded successfully. Signature enforcement was never disabled or bypassed.

Keep the private key outside this repository. Module enrollment changes the
host trust configuration and is therefore not performed by the experiment
wrapper. Rebuild and sign the module again whenever its contents or target
kernel changes.

## ABI files

- `include/uapi/uad2_compute.h`: fixed-width ioctl structures and capability
  bits
- `lib/uad2_compute.h`: userspace functions
- `lib/uad2_compute.c`: userspace implementation
- `kernel/uad2_compute.c`: PCI transport and guarded device operations
- `lib/uad2ctl.c`: diagnostic command-line client

The next ABI extension requires a real response from the device. Only then can
buffer handles, program handles, job IDs, completion waits, and ownership rules
be assigned kernel ioctls without baking guesses into a public interface.
The complete evidence gates and all-eight fault matrix are in
[`program-execution-gates.md`](program-execution-gates.md).
