# Official Windows reference-driver capture

This lab captures the first lifecycle of the exact UAD 11.0.1 Windows driver
while the OCTO is passed through to a disposable QEMU/KVM guest. It is an
evidence experiment, not part of the general-purpose Linux driver.

The guest uses an official Microsoft Windows 11 Enterprise evaluation image,
the official UAD installer packages, UEFI Secure Boot, an emulated TPM, and an
isolated VFIO group containing only `0000:03:00.0`. QEMU traces host-visible
PCI configuration, BAR access, interrupts, resets, and IOMMU mappings. The
PowerShell capture records the Windows PnP and service state before the UAD
packages, after installation, and after the required reboot.

The launcher disables direct VFIO BAR mapping and direct KVM interrupt
injection. These debug settings are slower, but they force MMIO and interrupt
traffic through QEMU so that the selected trace events actually observe it.

After writing `CAPTURE-COMPLETE.txt`, the guest also writes
`UAD2_CAPTURE_COMPLETE` to COM1. QEMU saves that marker in
`guest-serial.log` and leaves the guest running so the live DMA rings can be
inspected through the QEMU monitor. The operator must then request an orderly
guest shutdown.

## Files

- `Autounattend.xml` creates a disposable local administrator and starts the
  capture from media labeled `UAD2LAB`.
- `Capture-Uad2Lifecycle.ps1` reproduces the WiX bundle's system-32bit,
  system-64bit, and driver MSI order. It intentionally omits plug-ins, presets,
  and Apollo applications.
- `vfio-trace-events` is the QEMU trace allowlist.
- `run-reference-vm.sh` validates the exact endpoint, IOMMU group, Windows ISO
  hash, and explicit risk acknowledgment before binding VFIO and starting the
  traced guest.
- `run-collector-vm.sh` boots the same TPM-backed system disk without the PCIe
  card. It is used only to copy the completed Windows capture onto a separate
  output disk and cannot generate card traffic.
- `run-maintenance-vm.sh` boots a card-free guest with a read-only tools ISO.
  It permits host or plug-in installation without exposing the physical card.
- `Install-PluginHost.ps1` hash-locks the exact official UAD 11.0.1 plug-in
  packages and REAPER installer, records signature and file inventories, and
  installs them silently for the authorized plug-in reference trace.
- `RUN-INSTALL.cmd` is a one-click launcher for the plug-in-host installer and
  requests a clean shutdown when installation completes.
- `RUN-FINALIZE.cmd` revalidates already installed media, inventories the host,
  and requests a clean shutdown without reinstalling the packages.

The answer file contains a published temporary credential because the VM is
disposable. Keep the guest on QEMU user-mode NAT, do not expose inbound ports,
and destroy or change the account before reusing the disk for any other work.

## Safety boundary

Do not start the guest unless the endpoint identity and IOMMU group have been
rechecked immediately beforehand. Bind only `0000:03:00.0` to `vfio-pci`.
Do not pass another device from the host. The capture installs the official
signed driver and lets its normal PnP lifecycle run, but it does not invoke a
standalone FPGA or firmware updater.

The lifecycle may alter card state because the official driver owns reset and
firmware loading. A cold power cycle remains the recovery boundary. Preserve
the QEMU trace, Windows event logs, MSI logs, and before/after snapshots as one
atomic experiment record.

## Analysis

The raw traces, guest RAM, proprietary binaries, and installer media are not
public repository artifacts. Publish hashes and derived protocol facts only.
The dependency-free analyzers accept a legally captured local trace or ring
dump:

```bash
python3 tools/analyze_qemu_vfio_trace.py /path/to/qemu-vfio.trace \
  --writes-csv /tmp/uad2-writes.csv

python3 tools/analyze_uad2_ring_dump.py /path/to/rings.bin \
  --layout all-dsps

python3 tools/analyze_uad2_response.py /path/to/response-target.bin
```

For a live traced guest, `capture_response_boundary.py` tails the trace and
pauses QEMU on the first DSP0 response hardware read index at or above a
requested minimum. It then saves and hashes the actually consumed descriptor's
ring page and 4 KiB target:

```bash
python3 lab/windows/capture_response_boundary.py \
  /absolute/path/qemu-vfio.trace /absolute/path/boundary-capture \
  --target-index 3
```

The guest remains paused so the operator can inspect additional state before
issuing `cont` or an orderly shutdown through the QEMU monitor.

For a complete plug-in transaction, `capture_response_sequence.py` begins at
the current trace tail and pauses QEMU at every subsequent DSP0 command-host
write and response-read transition. A command boundary captures newly posted
bounded DMA objects before the official driver releases or zeroes them. A
response boundary captures every consumed response target. The tool writes
hashes, outer `Bill` and envelope metadata, ring indices, and response
classifications to JSON. Raw command objects remain in the private output
directory and must not be committed.

Response targets are sampled while the guest remains paused at 0, 5, and 100
ms after the target capture begins. The response ring and target are saved
before unrelated command pages. The total pause must remain below the official
loader's recovered 600 ms completion wait, otherwise the observer changes the
transaction it is intended to measure.

```bash
python3 lab/windows/capture_response_sequence.py \
  /absolute/path/qemu-vfio.trace \
  /absolute/path/response-sequence \
  --monitor-port 4444 --max-boundaries 64 --timeout 300
```

Run it after the runtime is idle and immediately before instantiating the one
authorized plug-in under test. A capture failure deliberately leaves QEMU
paused so potentially reused buffers are not lost.

The executed reference results and their public evidence boundaries are
documented in
[`docs/experiment-023-official-windows-reference.md`](../../docs/experiment-023-official-windows-reference.md)
and
[`docs/experiment-025-official-runtime-response.md`](../../docs/experiment-025-official-runtime-response.md).

## Authorization-state probe

`dump_auth_states.c` is a read-only probe for the official Windows client
library. It calls the same virtual method used by UADPerfMon to retrieve the
20-byte per-product authorization records and prints state counts plus the
non-default records. Build it as a 64-bit Windows executable and run it only
with the matching installed `UAD2DriverClient.dll`. It does not update an
authorization, submit a plug-in resource, or write a device register.
