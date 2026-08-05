# Experiment 020: read-only DSP resource-pool map

## Outcome

The four runtime resource pools and their reservations were recovered for all
eight DSPs. Every DSP reported the same layout. This experiment created no DMA
mapping and performed no MMIO write.

| Pool | Raw base | Raw size | Scratch | Usable start | Usable size | Direction |
|---:|---:|---:|---:|---:|---:|---|
| 0 | `0x00004000` | `0x000e0000` | `0x00000000` | `0x00004000` | `0x000e0000` | low to high |
| 1 | `0x00001fff` | `0x000c5000` | `0x00000080` | `0x00002080` | `0x000c4f80` | high to low |
| 2 | `0x0000a9b9` | `0x00092800` | `0x00000080` | `0x0000aa3a` | `0x00092780` | high to low |
| 3 | `0x00ff5cd2` | `0x0800832e` | `0x00008000` | `0x00ffdcd2` | `0x0800032e` | high to low |

The normalization follows the recovered `CResourcePool::Initialize` logic:
base and scratch are rounded up to an even dword, size is rounded down, then
scratch is removed from the free interval. Pool 0 allocates from the low end;
pools 1 through 3 allocate from the high end.

The registers are relative to each DSP register window:

- bases: type 0 at `+0x10`, type 2 at `+0x14`, type 1 at `+0x18`, and type 3
  at `+0x1c`;
- sizes: type 0 at `+0x184`, type 2 at `+0x188`, type 1 at `+0x18c`, and type
  3 at `+0x190`;
- reservations: type 2 at `+0x194`, type 1 at `+0x198`, and type 3 at
  `+0x19c`.

The per-DSP window is `(dsp > 3 ? 0x2000 : 0) + dsp * 0x800`. The snapshot was
taken through a read-only VFIO BAR mapping after exact PCI identity and IOMMU
group checks. The machine returned to an unbound, bus-master-disabled state.

The complete machine-readable result is
[`experiment-020-result.json`](experiment-020-result.json). The reproducer is
`tools/vfio_resource_layout.c` with the guarded
`tools/uad2-vfio-resource-layout.sh` wrapper.
