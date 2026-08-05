# Firmware family inventory

This note expands the exact OCTO analysis from one artifact to every
`FirmwareUpdate*.bin` object in the UAD 11.0.1 Windows installer. It records
metadata, hashes, and statistical comparisons only. No vendor bytes are stored
in the repository.

## Reproducible inventory

After extracting the MSI tables and files, run:

```bash
python3 tools/inventory_uad_firmware.py \
  /path/to/extracted-tables /path/to/extracted-files \
  > firmware-inventory.json
```

The inventory found 47 containers:

| Magic | Count | Working classification |
|---|---:|---|
| `FBUT` | 4 | legacy firmware-update family |
| `GBUT` | 33 | firmware-update family |
| `HBUT` | 10 | firmware-update family, including the tested OCTO |

All 47 satisfy `(word[6] + 16) * 4 == file_size`. This establishes a common
64-byte outer wrapper and exact declared-size rule across the installer set.
It does not establish a common inner codec.

## Header fields

Evidence supports the following conservative names:

| Word | Name | Evidence |
|---:|---|---|
| 0 | magic | literal `FBUT`, `GBUT`, or `HBUT` |
| 1 | build word | modern GBUT/HBUT values decode as plausible Unix timestamps |
| 2 | format-family word | modern objects use 42, legacy objects also use 10 or 11 |
| 3 | compatibility ID | exact OCTO value equals BAR `0x2218` |
| 4 | version word | varies by release or target, exact semantics unknown |
| 5 | platform word | target-correlated, exact semantics unknown |
| 6 | payload dwords | proves the exact file-size relation |
| 7 | constraint word | often `0xffffffff`, consumer unknown |
| 8 through 15 | opaque tail | no tested direct SHA-256 construction matches |

The exact OCTO build word `0x616e1720` decodes to
`2021-10-19T00:53:52Z`. The name `build word` remains intentional because old
FBUT values are counters rather than timestamps.

For every container, the 32-byte tail was compared with SHA-256 of:

- payload;
- the first 32 header bytes plus payload;
- payload plus the first 32 header bytes;
- the first 32 header bytes alone.

All four direct hypotheses fail for all 47 objects. This rules out only those
constructions. The tail could still be an encrypted digest, MAC, signature
fragment, nonce plus tag, or unrelated metadata.

## Pairwise evidence

Run `tools/compare_uad_containers.py` on locally extracted pairs. Three useful
comparisons are:

| Pair | Common payload bytes | Equal bytes | Equal 16-byte blocks | Longest equal run | XOR entropy |
|---|---:|---:|---:|---:|---:|
| OctoLuna2 HBUT vs QuadLuna2 HBUT | 3,930,424 | 0.003882278 | 0 | 2 | 7.999953 |
| DuoLavern2 GBUT vs QuadLavern2 GBUT | 4,341,672 | 0.003888133 | 0 | 3 | 7.999964 |
| OCTO HBUT vs OctoThatt HBUT | 2,557,576 | 0.003923246 | 0 | 2 | 7.999927 |

The near-random equality rate and high XOR entropy are statistically
consistent with separately encrypted or authenticated encodings. They do not
identify a cipher, mode, key, signature, compressor, or relocation format.
In particular, a high-entropy comparison cannot distinguish encryption from
compressed and randomized data.

## Consequences

The outer format is now inventoried, versioned, size-checked, and
compatibility-matched. The unresolved boundary is inside the 64-byte wrapper.
No safe host-side decoder can be implemented until one of these becomes
available:

1. the DSP or FPGA routine that consumes the opaque payload;
2. a lawful decoded reference plus its exact encoded source;
3. a vendor symbol or format description that establishes transforms and
   integrity fields.

Brute-forcing familiar ciphers against ciphertext alone is not a meaningful
next step. Consumer-focused data-flow analysis is.
