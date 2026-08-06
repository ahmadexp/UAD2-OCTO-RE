# Experiment 029: Bill integrity boundary

Controlled one-bit differentials now prove device-side integrity or
authentication over both parts of the opaque `0x12b` body.

## Layout

The complete DMA target contains an 8-byte allocation envelope followed by a
20-byte `Bill` header and a 432-byte body. The declared trailing length is 96
dwords, or 384 bytes. The body therefore consists of a 48-byte preserved
prefix and a 384-byte trailing core.

Each trial began with the exact captured target, flipped bit zero at one
approved offset in locked memory, submitted it without activating a module,
and then restored the endpoint. Unchanged targets were accepted before and
after the differential campaign.

## Results

| Target offset | Body offset | Region | DSP status | Public host result |
|---:|---:|---|---|---:|
| 28 | 0 | first prefix byte | `0xf0010005` | `-55` |
| 75 | 47 | last prefix byte | `0xf001000d` | `-60` |
| 76 | 48 | first core byte | `0xf001000d` | `-60` |
| 123 | 95 | core | `0xf001000d` | `-60` |
| 124 | 96 | core | `0xf001000d` | `-60` |
| 459 | 431 | last core byte | `0xf001000d` | `-60` |

Every mutation was rejected in 1 ms. The first prefix byte takes a distinct
validation branch; every other sampled prefix or core byte takes low code
`0x000d`. The unchanged object immediately returns zero status.

The fixed-commit public host driver maps low codes `0x0005` and `0x000d` to
host results `-55` and `-60`. It does not name their DSP-side meanings. The
hash-locked verifier is `tools/inspect_public_bill_status.py`.

## Conclusion

This is dynamic evidence for whole-body integrity or authentication. It rules
out treating the high-entropy trailing core as freely replaceable code and
rules out editing the preserved prefix independently. It does not identify the
cipher, MAC, signature algorithm, key source, clear executable, relocations, or
entry point. Those fields are either encrypted, authenticated, or both, and
remain consumed only inside the resident DSP framework.

See [`result.json`](data/experiment-029-bill-integrity/result.json).
