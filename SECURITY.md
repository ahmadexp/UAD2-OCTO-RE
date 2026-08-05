# Security and hardware-safety reports

This repository is experimental and provides no warranty. Do not run mutating
probes on a production audio workstation or a host containing unrelated devices
in the same IOMMU group.

For a vulnerability that could expose host memory, bypass an IOMMU boundary,
damage persistent card state, or leak credentials or hardware identifiers,
please use GitHub's private vulnerability reporting feature for this repository
instead of opening a public issue.

For a reproducible non-sensitive correctness bug, open a normal issue and
include the operating system, kernel, PCI identity, subsystem identity, BAR
size, IOMMU membership, exact command, and redacted output.

Never publish a board serial, access token, password, proprietary firmware, or
vendor driver binary in a report.
