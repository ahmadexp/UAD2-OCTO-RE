# Firmware and binary-container analysis

## Exact OCTO artifact

The official UAD 11.0.1 Windows installer contains a file named
`FirmwareUpdateOcto.bin`. The binary is not redistributed here. Its reproducible
identity is:

| Property | Value |
|---|---|
| Size | 2,558,096 bytes |
| SHA-256 | `f503787c0f253fc9713a47ae7e15adff242a6dde647dae7cb8ab6550ed976447` |
| Magic | `HBUT` |
| Header size | 64 bytes, 16 little-endian dwords |
| Compatibility ID | `0xa012dc0d` |
| Declared payload | 639,508 dwords |

The compatibility ID exactly matches BAR `0x2218` on the tested card. This is
direct evidence that the installer artifact targets this OCTO FPGA revision.

The 16 header dwords are:

```text
0: 0x54554248  "HBUT"
1: 0x616e1720
2: 0x0000002a
3: 0xa012dc0d  compatibility ID
4: 0x030f8598
5: 0x03000005
6: 0x0009c214  payload length in dwords
7: 0xffffffff
8..15: opaque 32-byte header tail
```

The exact size relation is `(word[6] + 16) * 4 == file_size`. The opaque tail
and high-entropy payload may participate in integrity, authentication,
encryption, compression, or a combination, but none of those roles is proven
yet. The repository deliberately does not label the bytes as a signature or
digest without a verified consumer.

## Loader dispatch recovered from UADPerfMon

Static analysis of the hash-identified UAD 11.0.1 `UADPerfMon` executable,
SHA-256
`18417006f5dab4e9f149d6f57e618e2c543f3ad1b9966b7be7052222086b9359`,
shows the following read-only classification path:

1. The updater reads the selected file and allocates a driver DMA buffer.
2. It copies the entire file into that buffer.
3. `FBUT`, `GBUT`, and `HBUT` select the firmware-update virtual method.
4. A separate magic selects an authentication-block method.
5. Other distinct magic values select demo-extension or reject paths.
6. The method releases the DMA buffer after the device call.

The matching `UAD2DriverClient.dll`, SHA-256
`da5c7925d89a9e43574abcfebc4b2e03dd3b0059bf08d5f423ee51d5df8f89da`,
exposes the next layer. Its driver-interface vtable uses three adjacent,
distinct operations:

| Interface method | Operation index | Windows control code |
|---|---:|---:|
| Authentication block | `0x67` | `0x001d219c` |
| Demo extension | `0x68` | `0x001d21a0` |
| Firmware update | `0x69` | `0x001d21a4` |

The firmware-update wrapper sends a 16-byte input record containing unit index,
byte count, and the pointer returned by the client's DMA-buffer allocator. It
expects a four-byte result. The dedicated operation confirms that `HBUT` is
not sent through the ordinary plug-in loader.

The symbolized official macOS driver resolves the dispatch boundary.
`CMessenger::_loadBlock` maps operation `0x69` to the device's `LoadFirmware`
virtual method. Operation `0x6a` maps to the distinct `LoadFPGAImage` method;
authentication and demo blocks are operations `0x67` and `0x68`. The
`CUAD2Device::LoadFirmware` implementation invokes `_sendBlock` with command
base `0x00120000`, expected response class `0x80040000`, and a 150,000 ms
timeout. Its helper uses command-ring DMA references and a four-dword response
descriptor. This proves that the HBUT-selected operation reaches the firmware
block helper, not the separate FPGA-image dispatcher. It does not prove that
the operation is volatile.

The exact updater executable also contains PCIe completion text requiring a
computer restart after the firmware update. Its `CUAD2Info::LoadBinFile` path
allocates a DMA buffer equal to the file size, copies the complete file,
checks the magic from that copy, and passes the same buffer and full byte count
to the firmware-update virtual method. It does not strip the 64-byte wrapper.
These facts require treating HBUT as potentially persistent despite its use of
the DSP command-ring helper.

In that executable, `CUAD2Info::LoadBinFile` begins at image address
`0x140162120`. The DMA allocation is called at `0x14016219d`, the complete copy
at `0x1401621e5`, magic is read at `0x1401621ea`, and the firmware-update
virtual call occurs at `0x14016228b`. The restart notice is stored at
`0x140497a80`. These image-relative observations are tied to the executable
hash above and make the full-file path reproducible without distributing it.

Two independent updater callers reach this path directly. The firmware-updater
virtual method passes the selected file, size, and unit straight to
`LoadBinFile`. The broader update workflow loops over units and makes the same
direct call. Neither caller invokes a separate block operation before or after
the `HBUT` call. Together with the one-method `UAD2System.sys` dispatch, this
rules out an automatic host-side `0x67`, `0x69`, `0x68` sequence. It does not
rule out a state machine inside the card or the DSP consumer.

The kernel device lifecycle contains a separate post-load distinction.
`LoadFirmware` sets a host flag before entering the block helper. A later hard
reset writes `0x0be0deaf` to DSP0 `+0x1a8` when that flag is set; without the
flag, it performs the ordinary BAR `+0x221c` reset pulse. Both Windows and
macOS implementations agree. This is not an adjacent loader command and does
not revive the speculative `0x67`, `0x69`, `0x68` sequence, but it may be a
firmware activation or commit boundary. Its device-side meaning is unresolved.

Experiment 018 submitted a one-dword zero placeholder and the 64-byte OCTO
HBUT header without its declared payload. Both command chains were consumed,
but neither produced a response. No complete firmware image was submitted.
See
[`experiment-018-loader-response-probes.md`](experiment-018-loader-response-probes.md).

Ordinary DSP programs use a separate `Bill` resource format. Its exact outer
parser, conditional host-side tail transform, and runtime allocator are documented in
[`bill-resource-analysis.md`](bill-resource-analysis.md). Those findings do not
decode the HBUT payload.

## Transformation and authentication matrix

| Payload | Host transformation | Host authentication | DSP-side status |
|---|---|---|---|
| `Bill`, payload form 0 | Complete byte-for-byte copy | No cryptographic check in the outer parser | Opaque and untested on OCTO |
| `Bill`, payload form nonzero | Replace declared trailing dwords with an ID-seeded deterministic stream | No cryptographic check in the outer parser | Opaque and untested on OCTO |
| `HBUT` firmware-update object | Fixed 64-byte wrapper parsed; inner payload unresolved | Unknown | Exact operation may be persistent; one complete chain stopped before the first data descriptor completed |
| Runtime loader block | Page-chained DMA framing recovered | Accepted-image validation unknown | Incomplete probes consumed without a reply |

This matrix distinguishes lack of a host-side check from proof that no
authentication exists. DSP firmware can still authenticate or decrypt the
opaque bytes after receipt.

## Comparative payload evidence

All 47 `FBUT`, `GBUT`, and `HBUT` containers in the installer obey the same exact size
relation, `(word[6] + 16) * 4 == file_size`. The OCTO HBUT payload has measured
Shannon entropy of 7.999936 bits per byte and contains no recognizable
plaintext executable magic. Same-size HBUT and GBUT variants match at about
one byte in 256, which is consistent with independent high-entropy ciphertext
or authenticated encodings. It is not enough to identify a cipher, key,
compression scheme, signature, or relocation model. Some same-size FBUT
variants retain large identical regions, so the three magic families cannot be
assumed to share one inner representation.

The metadata-only family inventory, direct SHA-256 tail tests, and reproducible
pairwise metrics are in
[`firmware-family-inventory.md`](firmware-family-inventory.md).

## Offline tools

`tools/inspect_uad_container.py` parses only the fixed header and hashes the
payload. It never accesses hardware:

```bash
python3 tools/inspect_uad_container.py /path/to/FirmwareUpdateOcto.bin
```

`tools/inspect_msi_tables.py` can inspect MSI metadata after a user-owned MSI
has been extracted into compound-document streams. Neither tool redistributes
vendor material.

`tools/inventory_uad_firmware.py` inventories the complete firmware family and
tests common direct SHA-256 tail constructions. `tools/compare_uad_containers.py`
compares opaque payload structure without attempting to decode or export it.

## What remains unknown

- Exact consumer semantics of header words 1, 2, 4, 5, 7, and the opaque tail.
- Payload transform and integrity algorithm.
- Public-key or symmetric authentication, if any.
- Segment and relocation records after decoding.
- Exact DSP-side meaning of the HBUT inner payload.
- Per-plug-in code and data overlay format.
- Entry point, ABI, and allocation rules for a harmless DSP0 program.

Experiment 021 submitted the exact hash-identified HBUT through the recovered
large-block framing. The device consumed the extended command header but did
not complete the first data descriptor or write a response. Cleanup and reset
fully recovered the card. Further hardware submission is paused until the
persistent update state machine can be excluded.

The response-state evidence and required next observations are separated in
[`runtime-response-state.md`](runtime-response-state.md).
