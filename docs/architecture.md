# Working architecture and path to generic compute

## What is established

Universal Audio describes OCTO as eight SHARC processors. The target endpoint
has a later-generation FPGA profile: PCI ID `1a00:0002`, a 64 KiB BAR, and
subsystem ID `0005`. Universal Audio separately notes that OCTO uses a later
FPGA generation than older SOLO, DUO, and QUAD cards.

References:

- [UA overview of UAD-2 DSP accelerators](https://help.uaudio.com/hc/en-us/articles/214356386-What-is-a-UAD-2-DSP-Accelerator)
- [UA note on the later OCTO FPGA generation](https://help.uaudio.com/hc/en-us/articles/205152333)
- [Open Apollo](https://github.com/rolotrealanis98/open-apollo)
- [Experimental uad2 Linux driver](https://github.com/stepbrobd/uad2)

The related public implementations and Experiments 013 through 016 establish
this host-side path:

```text
userspace
  -> bounded kernel DMA allocation
  -> FPGA command ring in BAR0
  -> FPGA DMA fetch of a 16-byte descriptor or larger command buffer
  -> DSP command consumer
  -> SHARC program or module load
  -> response ring and completion interrupt
```

Each ring entry is four little-endian 32-bit words. It can carry an inline
command or reference a host DMA buffer. Static analysis recovers a runtime
block loader using command base `0x00120000`, response class `0x80040000`, and an
opaque image. Its safe relationship to the exact `HBUT` updater container is
not established, so no complete image has been sent to the card. Experiment
018 submitted only deliberately incomplete, non-executable probes.

The ordinary program-resource path uses a separate `Bill` container and a
different completion ABI. Its 20-byte outer header and deterministic tail
replacement are recovered in [`bill-resource-analysis.md`](bill-resource-analysis.md),
but its preserved executable core remains opaque.

## What generic compute requires

Audio transport is not the hard requirement. The minimum useful stack is:

1. Reset or recovery that returns the FPGA and all DSPs to a known state.
2. Per-DSP command and response rings with IOMMU-contained DMA.
3. A loader for a program format accepted by the DSP firmware.
4. Host buffers with explicit ownership and cache synchronization.
5. A job ABI such as `program`, `input buffers`, `output buffers`, and
   `completion`.
6. Timeouts that reset only the failed DSP before escalating to card reset.

The first demonstration should be a heartbeat or buffer transform on DSP 0,
not simultaneous code on all eight processors. A good initial transform is
`out[i] = in[i] + constant`, because it proves code execution, input DMA,
output DMA, ordering, and completion without depending on floating-point edge
cases.

## Main unknowns

- Optical and electrical determination of boot straps and the exact core clock;
  the `ADSP-21469 KBCZ-00` device marking itself is now confirmed.
- Whether the FPGA or resident firmware authenticates program containers.
- Whether a vendor firmware image is mandatory before DSP ring commands work.
- The runtime framework image and loader command framing.
- Core-local recovery after a hung program. Empty-transport per-DSP engine
  reset and whole-card VFIO recovery are reliable.
- Whether the FPGA supports arbitrary host-to-DSP transfers or only a fixed set
  of firmware command types.

The likely processor family has public boot and DMA documentation. Analog
Devices documents that ADSP-21467/21469 programs can be booted through external
interfaces and downloaded with DMA. That establishes processor capability, but
it does not establish how this board wires or gates those interfaces.

- [Analog Devices ADSP-21469 product and documentation](https://www.analog.com/en/products/adsp-21469.html)
- [ADSP-21467/21469 data sheet](https://www.analog.com/media/en/technical-documentation/data-sheets/adsp-21467_21469.pdf)

## Userspace ABI

The implemented library keeps the early ABI deliberately small:

```c
int uad2_compute_open(unsigned card_index, struct uad2_compute **out);
int uad2_compute_get_info(struct uad2_compute *device,
                          struct uad2_compute_info *info);
int uad2_compute_get_dsp_status(struct uad2_compute *device,
                                unsigned dsp,
                                struct uad2_compute_dsp_status *status);
int uad2_compute_start_transport(struct uad2_compute *device);
int uad2_compute_reset_dsp(struct uad2_compute *device, unsigned dsp,
                           struct uad2_compute_reset *result);
int uad2_compute_stop_transport(struct uad2_compute *device);
```

Buffer, load, submit, and wait function names are also reserved in the library,
but return `-EOPNOTSUPP`. Their capability bits remain clear. See
[`driver-api.md`](driver-api.md). The public API exposes neither arbitrary MMIO
writes nor physical DMA addresses.
