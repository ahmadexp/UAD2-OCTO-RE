# Contributing

Contributions are welcome when they preserve the project's evidence and safety
standards. Documentation corrections, offline parsers, tests, and passive
captures are the best places to begin.

## Before submitting a change

```bash
python3 -m unittest discover -s tests -v
python3 -m py_compile tools/*.py
for file in docs/*.json; do python3 -m json.tool "$file" >/dev/null; done
for file in tools/*.sh; do sh -n "$file"; done
```

Keep code compatible with the exact identity checks unless the contribution
adds a separately documented hardware profile. Do not broaden an allowlist or
remove a refusal condition to make an experiment run.

## Proposing a hardware experiment

An experiment proposal should include:

1. The question answered by exactly one run.
2. Static or observed evidence for every register and value.
3. Exact endpoint and cold-state preconditions.
4. Complete IOVA mappings and maximum DMA reach.
5. The ordered write set and maximum wait time.
6. Acceptance, abort, cleanup, and independent recovery checks.
7. Explicitly excluded operations.

Submit the procedure for review before submitting code that performs the
writes. Never include passwords, tokens, serials, proprietary binaries,
firmware, installer extracts, or private host information.

## Result records

Store machine-readable results under `docs/experiment-NNN-result.json` and a
human-readable procedure beside them. Include the date, target PCI identity,
source hash, bounded resources, observed values, cleanup status, and whether
the expected observation succeeded. A negative result is valuable and should
not be relabeled as success merely because recovery worked.

## Licensing

By contributing, you agree that your contribution is licensed under
GPL-2.0-only, matching the repository.
