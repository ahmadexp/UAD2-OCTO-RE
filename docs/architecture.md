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

The related public implementations and Experiments 013 through 047 establish
this host-side path:

```text
userspace
  -> bounded kernel DMA allocation
  -> FPGA command ring in BAR0
  -> FPGA DMA fetch of a 16-byte descriptor or larger command buffer
  -> DSP command consumer
  -> authenticated SHARC resource allocation
  -> 64-sample channel buffers plus main Process command
  -> response ring and completion interrupt
```

Each ring entry is four little-endian 32-bit words. It can carry an inline
command or reference a host DMA buffer. Static analysis recovers a runtime
block loader using command base `0x00120000`, response class `0x80040000`, and an
opaque image. Its relationship to the exact `HBUT` updater container is now
captured through the official update, but the HBUT inner image remains opaque.

The ordinary program-resource path uses a separate `Bill` container and a
different completion ABI. Its 20-byte outer header, deterministic tail
replacement, allocation sequence, private-resource map, Process buffers, and
completion record are recovered in
[`bill-resource-analysis.md`](bill-resource-analysis.md), but its preserved
executable core remains opaque.

## What generic compute requires

Audio transport is not the hard requirement. The minimum useful stack is:

1. Reset or recovery that returns the FPGA and all DSPs to a known state.
2. Per-DSP command and response rings with IOMMU-contained DMA.
3. A loader for a program format accepted by the DSP firmware.
4. Host buffers with explicit ownership and cache synchronization.
5. A job ABI such as `program`, `input buffers`, `output buffers`, and
   `completion`.
6. Timeouts that reset only the failed DSP before escalating to card reset.

The first official-workload demonstration is complete: one opposed half-scale
stereo impulse returns exactly through a 64-sample tick on every DSP. The next
demonstration must use user-authored code, such as `out[i] = in[i] + constant`,
because only that would prove control over the instruction stream.

## Main unknowns

- Optical and electrical determination of boot straps and the exact core clock;
  the `ADSP-21469 KBCZ-00` device marking itself is now confirmed.
- The inner authentication or encryption algorithm and key source.
- Inner segment, relocation, entry-point, and runtime-reservation records.
- Core-local recovery after a hung custom program. Normal authorized-job and
  device-rejection recovery are reliable.
- Whether a lawful development format can create a new accepted Bill object.

The likely processor family has public boot and DMA documentation. Analog
Devices documents that ADSP-21467/21469 programs can be booted through external
interfaces and downloaded with DMA. That establishes processor capability, but
it does not establish how this board wires or gates those interfaces.

- [Analog Devices ADSP-21469 product and documentation](https://www.analog.com/en/products/adsp-21469.html)
- [ADSP-21467/21469 data sheet](https://www.analog.com/media/en/technical-documentation/data-sheets/adsp-21467_21469.pdf)

## Userspace ABI

The version-2 library exposes the validated operations:

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
int uad2_compute_alloc_buffer(struct uad2_compute *device, size_t bytes,
                              unsigned flags, uint64_t *buffer_id);
int uad2_compute_load_program(struct uad2_compute *device, unsigned dsp,
                              const void *image, size_t bytes,
                              uint64_t *program_id);
int uad2_compute_submit(struct uad2_compute *device, unsigned dsp,
                        uint64_t program_id, uint64_t input_buffer_id,
                        uint64_t output_buffer_id, uint64_t *job_id);
int uad2_compute_wait(struct uad2_compute *device, uint64_t job_id,
                      int timeout_ms,
                      struct uad2_compute_job_wait *completion);
```

The program loader accepts only the exact authenticated RealVerb bundle. Submit
is synchronous and wait retrieves the completed record. See
[`driver-api.md`](driver-api.md). The public API exposes neither arbitrary MMIO
writes, physical DMA addresses, raw commands, nor arbitrary program images.
