# Experiments 048 through 050: allocation metadata and runtime readback

## Result

The official Process-coupled resource-readback transaction now returns a valid
DSP response. A complete bounded snapshot of RealVerb's first private resource
then recovered its 430-dword runtime control block. All 398 ordinary words are
zero, and its 32 nonzero words occur exactly at the destination offsets from
the host memory-spec update. Their values are exactly the 32 later allocation
addresses.

This identifies the Process object at `0x0009d00a` as a zero-initialized
control block patched by the host. It is not a hidden clear SHARC executable.
A negative control aimed at the first public Bill allocation consumed the
readback command but did not consume its response descriptor or change its
canary buffer. The official readback mechanism is therefore not an
unrestricted public-pool memory oracle.

The sanitized result is
[`data/experiment-048-050-runtime-readback/result.json`](data/experiment-048-050-runtime-readback/result.json).
It includes hashes and derived metadata, but no proprietary plug-in or DSP
payload bytes.

## Experiment 048: exact native allocation record

The exact installed files used in the isolated Windows reference VM were:

| File | SHA-256 |
|---|---|
| `UAD2SDK.dll` | `ca09fd717d6d28a5cb747c8b8c587a1baf27ab973139410379a887517470de8b` |
| `UAD2DriverClient.dll` | `da5c7925d89a9e43574abcfebc4b2e03dd3b0059bf08d5f423ee51d5df8f89da` |
| `UAD RealVerb-Pro.dll` | `26b27e8e5e31bdae054ba748f559d874332f4281760d6723ca6f1c93d42c61e8` |

Static analysis shows that `UAD2SDK.dll` constructs the native allocation
record on its stack, invokes the plug-in callback that fills it, and passes the
same record to `CreateUAD2PlugIn2`. A small forwarding DLL recorded that
argument immediately before forwarding the original pointer and status
arguments unchanged. Its source is
[`tools/windows/uad2_alloc_capture_proxy.c`](../tools/windows/uad2_alloc_capture_proxy.c).
It does not alter authorization, Bill objects, command rings, DSP traffic, or
the official factory's return value.

Two independent captures have different raw hashes because they contain
process-local pointers. After all resource and chain pointers are set to zero,
both records have SHA-256
`a9a07e027023eb90a056747a13905e6acc6737888e3a5f60093fbac26c3ad0e0`.
The capture confirms these exact native fields:

| Field | Value |
|---|---:|
| native record length | `0x0af8` bytes |
| public resource count | 26 |
| memory-spec count at `0x188` | 33 |
| memory-spec entry length | 16 bytes |
| readback count at `0x98c` | 1 |
| readback entry length | 8 bytes |
| readback resource | 0 |
| readback dword offset | 419 (`0x1a3`) |
| readback dword count | 4 |

The live length corrects the earlier `0x0af0` estimate. The first memory spec
describes a 430-dword private allocation. The remaining 32 entries select
destination offsets in that object and provide the lengths and flags for later
allocations. The full raw tuples are published as derived metadata in the
sanitized JSON. The generic pointer-scrubbing decoder is
[`tools/inspect_plugin_alloc_capture.py`](../tools/inspect_plugin_alloc_capture.py).

## Experiment 048: first valid readback response

Earlier Experiments 031 and 032 sent readback after resource loading but
outside a Process transaction. The command was consumed without a response.
Hash-locked driver analysis showed the missing condition: Process sets flag
bit 1 when readbacks are attached and queues the readback response in the same
job. The reproduced Process command is:

```text
000b0004 00400002 00000001 0009d00a
```

The captured readback spec resolves private resource 0, offset `0x1a3`, to
address `0x0009d1ad`. The exact command and response were:

```text
command   000c0004 0009d1ad 00000004 000001a3
response  80010006 000001a3 00000000 00000000 00000000 00000000
```

The command and response descriptors were both consumed. The response header
encodes six dwords, word one echoes the packed resource spec, and the four
requested payload dwords are zero. The normal two-channel Process responses
also completed and all writes remained inside their declared prefixes.

This closes the valid-readback-response gate. It does not expose Bill code
because the operation applies to the plug-in's mapped private resource.

## Experiment 049: complete private runtime object

The first memory spec fixes the private resource length at 430 dwords, so the
next trial requested exactly that known range and no more:

```text
command   000c0004 0009d00a 000001ae 00000000
header    800101b0 00000000
```

The 430-dword payload has little-endian SHA-256
`6867505cb82b637181cb4ac59e728d210b65ee47571a3490239d80ab294f3d21`.
It contains 398 zero dwords and 32 nonzero dwords. The nonzero offsets are:

```text
168 169 170 178 179 180 181 182 183 184 185 186 187 188 189 190
191 192 196 197 198 220 231 232 233 234 235 236 416 417 418 426
```

These are exactly, with no additions or omissions, the 32 destination offsets
in the later memory specs. The first 31 values point at other private
allocations in the `0x0009c...` range. The final value is the large allocation
at `0x08fee380`. This dynamically confirms the complete host relocation layer:

```text
private_object[destination_offset] = later_allocation_address
```

No instruction words, clear segment headers, entry point, or embedded Bill
body appear in this control block. The runtime object is still valuable: it
defines the Process-facing state layout and gives a bounded way to verify host
patches. It does not provide the executable needed for a user-authored kernel.

## Experiment 050: public Bill readback negative control

The same Process-coupled operation requested 64 dwords starting at the known
first public Bill allocation, `0x000e0000`. The two normal audio response
objects completed. The readback command was consumed, but after the fixed
6,000 ms deadline its response descriptor was not consumed and all 66 response
dwords retained the `0xa5a5a5a5` canary.

This is a clean negative result. It rules out using the recovered operation as
a generic read primitive for the public Bill pool. The trial did not scan
unknown memory and did not request beyond the known allocation.

## Standard SHARC loader hypothesis

Analog Devices documents ordinary SHARC loader blocks as three 32-bit header
words containing a tag, count, and target. The documented tags include
`FINAL_INIT`, zero-fill forms, and initialized 16-, 32-, 48-, and 64-bit data
forms. The ADSP-21469 memory map also places internal RAM in blocks including
the observed `0x0009...` and `0x000e...` regions. See the
[CCES Loader and Utilities Manual](https://www.analog.com/media/en/dsp-documentation/software-manuals/cces-loaderutilities-manual.pdf),
[ADSP-21469 EZ-Board manual](https://www.analog.com/media/en/technical-documentation/user-guides/ADSP-21469_ezboard_man_rev.1.1.pdf),
and [Analog Devices' ADSP-21469 page](https://www.analog.com/en/products/adsp-21469.html).

`tools/analyze_bill_resources.py` now scans the exact post-transform Bill wire
bodies for plausible loader headers at every byte offset in both 32-bit word
endiannesses. Across the 26 RealVerb resources it tested 45,398 byte offsets
and found zero candidates. This rejects clear standard LDR framing in this
corpus. It does not prove one specific encryption or authentication algorithm.

## Safety and recovery

Every Linux trial used the exact endpoint and subsystem allowlist, required an
isolated one-device IOMMU group, refused pre-existing bus mastering, verified
all private resource hashes, bounded every DMA target, and enforced a fixed
deadline. After each trial:

- all eight DSP-ready bits were set;
- every non-target ring index was unchanged;
- writes were confined to declared response prefixes;
- explicit restoration succeeded;
- `VFIO_DEVICE_RESET` recovered the cold ring state;
- the complete IOMMU range was unmapped; and
- the endpoint was left unbound.

## Consequence for affine, biquad, and convolution kernels

The host side is now sufficiently understood for a future custom kernel: it
can allocate bounded buffers, patch a private control object, submit a Process
job, read completion data, target any one of eight DSPs, and recover. The card
still cannot accept a user-authored executable because the authenticated inner
Bill format and its DSP-side consumer remain opaque.

The next successful milestone must be one of these, in descending evidentiary
value:

1. capture a lawful post-decode program image and its entry state through a
   documented debug or development path;
2. identify a vendor development container that the resident framework accepts
   for this exact ADSP-21469 generation;
3. recover the Bill decoder from a clear framework image or a bounded
   instruction/data trace; or
4. obtain enough cross-version chosen-corpus evidence to identify the lawful
   signing or packaging rules without extracting a secret.

Once that gate closes, the first program should be a 64-sample affine transform
with fixed coefficients and canary-bounded output. A one-section biquad should
follow to validate persistent state, then a short FIR. Long partitioned
convolution should wait until the memory and overlay ABI is known. Calling the
current card a general-purpose GPU would be inaccurate: it is an eight-engine
SHARC accelerator with an audio-oriented command and buffer pipeline.

## Bounded next probe result

Experiments 053 and 054 completed the required official framework refresh and
the one bounded selector-1 retry. The exact sequence succeeded, and allocation
index 1 returned a valid 150-dword payload containing only zeros. See
[`experiment-053-054-framework-refresh-selector1.md`](experiment-053-054-framework-refresh-selector1.md).
