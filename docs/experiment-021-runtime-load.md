# Experiment 021: exact OCTO HBUT chain

> [!NOTE]
> Experiment 023 later completed the full chain through the unmodified official
> Windows updater. Experiment 021 remains the bounded Linux result, but its
> first-payload stop is no longer the final evidence about the official
> framing. See
> [`experiment-023-official-windows-reference.md`](experiment-023-official-windows-reference.md).

## Outcome

The first complete submission of the exact, compatibility-matched OCTO HBUT
did not produce a loader completion. The previously validated connect commands
were consumed. The hardware read index then advanced from 2 to 3, proving that
it consumed the extended header descriptor, and stopped on the first HBUT data
descriptor. No response byte changed and every DSP ready bit remained set.

The experiment used the large-block form recovered from `_sendBlock`:

```text
header[0] = 0x40120000
header[1] = 0x0009c226
625 page-bounded DMA references carry the 2,558,096-byte HBUT
expected response class = 0x80040000
```

The wrapper required SHA-256
`f503787c0f253fc9713a47ae7e15adff242a6dde647dae7cb8ab6550ed976447`,
the C probe independently checked the exact size, `HBUT` magic, declared
payload length, and compatibility ID, and the live BAR revision matched
`0xa012dc0d` before any write.

The IOMMU exposed only 691 locked pages containing the 64 ring pages, one
response page, one header page, and the payload pages. Memory outside the
four-dword response prefix was unchanged. Explicit ring and DMA restoration,
VFIO function reset, complete IOMMU unmap, VFIO unbind, and the independent
post-run bus-master check all succeeded.

This negative result is not evidence that the HBUT failed authentication
because the response path was never used. Static analysis after the run found
that the official updater copies the complete file into its DMA buffer and
sends its full byte count unchanged, so stripping the 64-byte wrapper is not
the missing step.

The same updater contains explicit PCIe completion text requiring a computer
restart. Although operation `0x69` and the DSP command-ring block helper are
distinct from operation `0x6a` (`LoadFPGAImage`), this does not prove that the
HBUT operation is volatile. No further complete or malformed HBUT submission
is justified until the persistence and update-mode state machines are
separated conclusively.

The reproducer is now disabled by default. Its wrapper requires an explicit
`UAD2_ALLOW_PERSISTENT_FIRMWARE_EXPERIMENT=YES_I_ACCEPT_CARD_FIRMWARE_RISK`
acknowledgment in addition to root, the exact artifact hash, the exact PCI
identity, disabled bus mastering, and isolated IOMMU group checks. This gate is
not a claim that another run is technically justified.

The machine-readable record is
[`experiment-021-result.json`](experiment-021-result.json).
