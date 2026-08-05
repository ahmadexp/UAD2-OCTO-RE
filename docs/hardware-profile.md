# Tested hardware profile

The repository targets one Universal Audio UAD-2 OCTO PCIe board marked
`UAD-2 OCTO PCB 50-03217`, assembly `80-13023 Rev 5`.

![UAD-2 OCTO Rev 5 test card](images/uad2-octo-rev5.jpg)

## PCI endpoint

| Property | Observed value |
|---|---:|
| Vendor and device | `1a00:0002` |
| Subsystem vendor and device | `1a00:0005` |
| PCI class | `0480`, multimedia controller |
| Revision | `0x00` |
| BAR0 | 64 KiB memory window |
| Link | PCIe 2.5 GT/s x1 |
| FPGA revision | `0xa012dc0d` |
| Extended capabilities | `0x00300811` |
| Reported DSP count | 8 |
| Reported family field | 3 |

The passive capture found no bound driver, PCI memory decoding disabled, bus
mastering disabled, and the endpoint isolated in IOMMU group 16. Group numbers
and BDFs are host-specific and must not be assumed on another machine.

## DSP-ready profile

DSP0 reports `0x00000a03`; DSP1 through DSP7 each report `0x00000003`. Ready
bit zero is set and values were stable across repeated reads. Exact locations
are recorded in [`dsp-status-2026-08-04.json`](dsp-status-2026-08-04.json).

## Privacy and provenance

The board serial was observed but is not stored. The photograph shows the
actual research card and has its barcode obscured. Captured host addressing is
redacted because it is irrelevant to reproducing the hardware findings.

## Compatibility boundary

Do not treat `1a00:0001` cards, 16 KiB BAR devices, other subsystem IDs, Apollo
interfaces, or Satellite units as interchangeable. Every mutating wrapper in
this repository checks the tested endpoint identity and expected IOMMU
isolation before binding VFIO.
