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
It does not establish that the file is a volatile DSP runtime image. The
official updater labels it a firmware update and presents a do-not-power-off
warning, so it must be treated as potentially persistent.

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
not sent through the ordinary plug-in loader. It still does not reveal whether
the device ultimately writes flash, configures the FPGA, or stages another
persistent component, so hardware execution remains prohibited.

The official macOS driver independently shows a runtime `LoadFirmware` path
using command base `0x00120000`, expected response class `0x80040000`, and a
150,000 ms timeout. Its block helper uses command-ring DMA references and
response descriptors. The relationship between that runtime path and the
updater's potentially persistent `HBUT` object is not yet proven.

Experiment 018 submitted a one-dword zero placeholder and the 64-byte OCTO
HBUT header without its declared payload. Both command chains were consumed,
but neither produced a response. No complete firmware image was submitted.
See
[`experiment-018-loader-response-probes.md`](experiment-018-loader-response-probes.md).

Ordinary DSP programs use a separate `Bill` resource format. Its exact outer
parser and deterministic host-side tail transform are documented in
[`bill-resource-analysis.md`](bill-resource-analysis.md). Those findings do not
decode the HBUT payload.

## Offline tools

`tools/inspect_uad_container.py` parses only the fixed header and hashes the
payload. It never accesses hardware:

```bash
python3 tools/inspect_uad_container.py /path/to/FirmwareUpdateOcto.bin
```

`tools/inspect_msi_tables.py` can inspect MSI metadata after a user-owned MSI
has been extracted into compound-document streams. Neither tool redistributes
vendor material.

## What remains unknown

- Meaning of header words 1, 2, 4, 5, 7, and the opaque tail.
- Payload transform and integrity algorithm.
- Public-key or symmetric authentication, if any.
- Segment and relocation records after decoding.
- Runtime DSP framework container versus persistent FPGA update boundary.
- Per-plug-in code and data overlay format.
- Entry point, ABI, and allocation rules for a harmless DSP0 program.

The next safe step is offline recovery of the driver virtual method that
consumes the `HBUT` buffer and of the runtime block loader. Programming this
file into the card is outside the current safety boundary.
