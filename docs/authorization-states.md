# Plug-in authorization response and state meanings

## Result

The authorization response is now decoded from the exact UAD 11.0.1 host
binaries. The four distinct dwords present in the captured OCTO table do not
represent four distinct display meanings. Two are separate authorized wire
encodings that the official driver deliberately collapses to one state.

| Raw table dword | Internal state | Official display meaning | Entries on this OCTO |
|---:|---:|---|---:|
| `0x80000000` | 2 | Demo not started | 203 |
| `0x81000000` | 0 | Authorized | 2 |
| `0x82000000` | 0 | Authorized | 24 |
| `0x83000000` | 4 | Auth update required | 539 |

The current 768-entry table therefore contains 26 authorized products, 203
products whose demos have not started, and 539 products requiring an
authorization-table update. No active or expired demo value occurs in this
capture.

The host interface exposes no distinction between `0x81000000` and
`0x82000000`. Both branches write internal state zero and the same auxiliary
value, `0xffffffff`. Calling them purchased, bundled, promotional, or any other
license category would be speculation. They are documented here only as two
authorized wire encodings.

## Complete decoder

The exact `UAD2Pcie.sys` decoder also covers values absent from the current
table:

| Raw condition | Internal state | Official display meaning |
|---|---:|---|
| `0x00000000` | 3 | Demo expired |
| `0x80000000` | 2 | Demo not started |
| `0x81000000` or `0x82000000` | 0 | Authorized |
| `0x83000000` | 4 | Auth update required |
| any other value | 1 | Demo active |

For an active demo, the official driver derives remaining days as:

```text
days = ((raw >> 12) & 0x7ffff) + (1 if (raw & 0xfff) != 0 else 0)
```

This is a rounded-up conversion: the lower 12 bits act as a fractional-day
remainder. `UADPerfMon` accepts internal states one and three on its demo path,
uses the record's day field for an active demo, and selects `Demo expired` when
no days remain. A separate helper tests state four and selects `Auth update
required`.

## Response authentication and transport

The card request and response path is exact:

1. `UAD2Pcie.sys` initializes all 768 cached entries to `0x83000000`.
2. It submits inline commands `0x00100002` and `0x00110002`, each with a
   request token.
3. It accepts a response only when word zero is `0x80030302` and word one
   matches the request token.
4. It copies exactly 3,072 bytes, or 768 dwords, from the response table.

This token comparison authenticates response correlation, not a license
signature. No host-side code in this path verifies or changes authorization
credentials. The read-only probe in `lab/windows/dump_auth_states.c` calls the
official client interface and prints its 20-byte per-product records. It does
not submit an authorization update, a plug-in resource, or a device-register
write.

`UAD2System.sys` then merges the per-device 20-byte records. Authorized state
zero wins and records a device bit, active-demo state one retains the minimum
remaining days, state three wins over non-authorized demo states, and state
four is the default. This merge independently confirms the display semantics.

## Reproduce the static result

Obtain the exact binaries from a user-owned UAD 11.0.1 installation, then run:

```bash
python3 tools/inspect_authorization_states.py \
  /path/to/UADPerfMon /path/to/UAD2System.sys /path/to/UAD2Pcie.sys
```

The verifier refuses binaries whose SHA-256 hashes differ and checks the
decoder, table refresh, response-token comparison, multi-device merge, helper
functions, and UI strings. It reports only instruction-level conclusions and
contains no mechanism to modify authorization state.

## Boundary

This work names and reproduces the host-visible states. It does not recover the
source or business meaning of the two authorized subtypes, the server-side
authorization format, or any device-side signature scheme. Those questions are
unnecessary for lawful general-purpose compute work and remain outside the
project's execution path.
