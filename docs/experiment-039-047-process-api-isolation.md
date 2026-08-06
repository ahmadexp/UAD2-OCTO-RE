# Experiments 039 through 047: Process buffers, Linux API, and isolation

## Result

The project now reproduces the authenticated RealVerb allocation sequence and
one complete 64-sample stereo `Process` transaction. Linux supplies bounded
input buffers, receives two counter-correlated output buffers, and restores the
card. The transaction succeeds independently on DSP0 through DSP7 through both
the VFIO reproducer and the version-2 kernel API.

This is a real program-buffer execution milestone, but it is not arbitrary
code execution. The accepted program is the exact licensed RealVerb resource
set selected by the official runtime. With the captured default state, output
is an exact copy of the opposed half-scale impulse and seven later zero ticks
remain zero. No wet reverb tail appears. The experiment therefore proves the
framework's program-visible buffer and completion path, not execution of the
RealVerb effect algorithm or a user-authored SHARC entry point.

The sanitized machine-readable result is
[`data/experiment-039-047-runtime-api/result.json`](data/experiment-039-047-runtime-api/result.json).
Private captures are identified only by SHA-256.

## Experiment 039: exact RealVerb corpus

The recovered UAD 11.0.1 RealVerb DLL has SHA-256
`26b27e8e5e31bdae054ba748f559d874332f4281760d6723ca6f1c93d42c61e8`.
It contains 26 adjacent `Bill` objects: 13 generation-1 objects and 13
generation-2 objects. The generation-2 ID sequence exactly matches the 13
resources seen in the hardware transaction:

```text
12b eb c1 a5 120 bd 11f d0 f9 bf 11e 11d d1
```

Every pair preserves the same prefix size and nine pairs preserve the same
total size, yet their overlapping cores have only 0.3178 percent equal bytes
on average. Pairwise XOR entropy averages 7.588078 bits per byte. No standard
decompressor succeeds, no aligned 16-byte block repeats or crosses resource
boundaries, and the tested standard digest layouts match nothing. Combined
with the mutation rejections from Experiment 029, this strongly supports a
generation-specific authenticated or encrypted body. It does not identify the
algorithm, key source, segment format, relocation records, or entry point.

## The corrected Process object

Experiments 040 through 042 initially placed `0x000e0000` in word three of the
main Process command. That value is the first public `Bill` pool allocation.
It is not the plug-in Process object. Hardware still emitted response-shaped
data, but the zero and impulse inputs produced identical audio hashes and
nonzero garbage tails. Those trials are superseded controls and are not
program-execution evidence.

Hash-locked static analysis of `CPluginInstance::SetDSPResourceManager` shows
that plug-in object field `+0x0bd8` stores the mapped address of the first
private resource. `CPluginInstance::Process` copies that field into word three
of the main command. The first captured private allocation is `0x0009d00a`, so
the validated main command is:

```text
000b0004 00400000 request_id 0009d00a
```

This rule is now checked by
[`tools/inspect_program_runtime_abi.py`](../tools/inspect_program_runtime_abi.py)
and regression tests. It is distinct from the public Bill allocation range at
`0x000e0000`.

## Experiments 043 and 044: exact buffer and completion ABI

Each 64-sample stereo tick uses two 66-dword input objects and two 68-dword
response objects. Input object words zero and one carry the channel command
and property address; words two through 65 carry 64 raw sample dwords. Each
output starts with:

```text
80020044 request_id channel f001000e
```

The remaining 64 dwords are output samples. `0xf001000e` is the repeatable
hardware Process marker for this path. Its internal semantic name is not
known, so this project does not label it an error or success code beyond the
observed transaction.

The corrected zero control returned 64 zero samples on both channels. The
corrected impulse trial supplied `0x3f000000` (positive 0.5) to channel zero
and `0xbf000000` (negative 0.5) to channel one. Both values appeared exactly
in output sample zero and every other sample was zero. Experiment 044 then ran
eight ticks: one impulse tick followed by seven zero ticks. Response counters
advanced from one through eight, all channel IDs matched, every later output
was zero, and no tail appeared.

The result proves all of the following:

- exact input-object length and sample offset;
- exact output-object length and sample offset;
- channel selection and request correlation;
- main command length, flags, counter, and private-resource pointer;
- bounded writes to the declared response prefixes;
- completion visibility before cleanup; and
- exact reset and IOMMU teardown after the stream.

It does not prove a wet RealVerb configuration. The unchanged samples are
consistent with a default, bypassed, or unconfigured processing state.

## Experiment 045: all-eight VFIO isolation

The corrected impulse transaction was repeated once for each target DSP. All
13 resources were accepted in every trial. Each target returned the exact two
sample values and correlated completion headers. All seven non-target command
and response read indices remained unchanged per trial. All writes stayed in
the allowed response regions, all DSPs remained ready, explicit restoration
passed, VFIO reset passed, and every IOMMU unmap reported the complete range.

This closes normal program-level targeting and recovery across all eight
engines. It does not close hostile-program isolation because the kernel does
not accept arbitrary program images and no out-of-range DSP write primitive is
available.

## Experiment 046: Linux ABI version 2

The kernel module and userspace library now expose these validated operations:

- coherent input and output buffer allocation with opaque handles;
- bounded `mmap` of kernel-owned buffers;
- exact authenticated RealVerb bundle validation and loading;
- one synchronous 64-sample stereo job submission;
- request-correlated completion retrieval; and
- per-job non-target ring-index checks.

The capability bitmap is `0x3f`: ring transport, per-DSP reset, authorized
program load, DMA buffers, job completion, and program isolation. The UAPI
does not expose BAR mappings, unrestricted commands, physical addresses, or
arbitrary program images.

The private 69,632-byte bundle is assembled from 16 exact page-bounded
transport chunks and one exact 65-dword private-resource memory specification.
[`tools/pack_realverb_program.py`](../tools/pack_realverb_program.py) checks the
SHA-256 of every source and marks the output as private. The bundle itself is
not committed.

The signed module loaded on the Secure Boot host without disabling signature
enforcement. Through `uad2ctl`, DSP0 through DSP7 each accepted the bundle,
completed one exact impulse roundtrip, stopped, and restored all ring indices
to zero. The module then unloaded and left the endpoint unbound.

The release audit repeated this all-eight trial after adding explicitly aligned
64-bit UAPI fields, response and input canaries, and PCI-removal mapping
revocation. The kernel built with `W=1`, the library built with
`-Wall -Wextra -Werror`, and the client compared all 512 output bytes against
the input on every target. All eight comparisons passed. The module was removed,
the endpoint was left unbound, and the IOMMU remained in `DMA-FQ` mode.

Submission is synchronous in ABI version 2. `WAIT_JOB` reports the most recent
already-completed job. It is a completion record, not an asynchronous queue.
Only one file may hold the device at a time, one authorized program may be
loaded at a time, and close stops transport and releases owned state.

## Experiment 047: rejection and recovery

One byte at bundle offset 100 was changed. The kernel submitted the
structurally valid but altered resource to the device, which rejected it. The
loader returned `EKEYREJECTED`, stopped transport, cleared the program, and
restored the cold state. An unchanged bundle then loaded immediately and
completed the exact impulse roundtrip. All eight ready bits were set and every
command and response index was zero afterward.

This establishes that the kernel does not treat its structural parser as the
authentication authority. The card remains authoritative for the proprietary
inner object.

## What remains for full arbitrary-code reverse engineering

The host transport, runtime allocation sequence, normal buffer exchange,
completion handling, Linux API, normal all-eight isolation, and recovery are
now implemented. The remaining hard boundary is the opaque inner `Bill` and
HBUT payload consumer:

- authenticated cleartext or a lawful creation mechanism for new objects;
- inner segment and executable records;
- DSP-side relocation rules;
- entry point and call ABI for user-authored code; and
- runtime stack, interrupt, circular-buffer, and overlay reservations.

Without those facts, the card can run only captured official workloads through
the new API. Calling that general-purpose SHARC programming would overstate the
evidence.
