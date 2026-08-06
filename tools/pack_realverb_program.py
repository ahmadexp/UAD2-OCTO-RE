#!/usr/bin/env python3
"""Pack the exact private RealVerb capture into the version-2 driver image.

The resulting bundle contains proprietary authenticated resources and must stay
private. This tool verifies every source hash, pads each transport chunk to one
4 KiB slot, and writes the 65-dword memory specification into slot 16.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


PAGE = 4096
CAPTURE_NAME = "experiment-029-deadline-safe-sequence"
CHUNKS = (
    ("boundary-0000/command-0002-target.bin", "0c353512fb27ed961b4e0746de7f1bbc462447f6e2c0263bc6209cda7b7718d0"),
    ("boundary-0002/command-0003-target.bin", "6d91985233b00edfad9c3e93c17bb75c7e51922d11076fec03d3ab23f3c99e21"),
    ("boundary-0004/command-0004-target.bin", "87512f74b674462241144deca658f59b165cee7e3d5635b1e0864ffecd284fb1"),
    ("boundary-0006/command-0005-target.bin", "0c3561eb68e83169d38e58cd70d6bc9464afbd4efc2b3ac34bc339e19cffe79a"),
    ("boundary-0008/command-0006-target.bin", "e2f918628e4c4882e142113a86f3ee9fcf8034a5ec48c35b663bf0f839f60ed0"),
    ("boundary-0010/command-0007-target.bin", "bd62be051299bfea36e54119643fea6089423aea6a0b060b8269d4f572c2c78e"),
    ("boundary-0012/command-0008-target.bin", "203be752a4d3fd8a11739484d3ad0af9ac26359fd1bec5850ef38dd0e1fd45ac"),
    ("boundary-0014/command-0009-target.bin", "f4c041ab7b0aa19ab9f32235e1b51f4db5b3f7f88fb3f526254b2a906ff96123"),
    ("boundary-0016/command-0010-target.bin", "07177bd6b15ba0dceafad3ba3d12147f0f87040cdd0b291abb1bca9cfdeb5783"),
    ("boundary-0016/command-0011-target.bin", "e813ef001a733114e9d775e4b8a5f4f0a36e889088ed6cf9162ee12675ff69de"),
    ("boundary-0018/command-0012-target.bin", "884f3682122e2dad85895eda6df3545c657c8b0dc2c65d71ddd68a3ae130e7d5"),
    ("boundary-0018/command-0013-target.bin", "09a0f5d881656044220c0dfe9e8568411ea5e41c192e357aa49872bd37efda75"),
    ("boundary-0020/command-0014-target.bin", "8e6f5a294bde687a7eeeab2d5a447666d289080d666c63f5e72724531caa6106"),
    ("boundary-0022/command-0015-target.bin", "ea339b46f1a8d871ac8122d01ce151289a415aa0fd23e6ff0d0840411338667b"),
    ("boundary-0024/command-0016-target.bin", "17d79a7db9af334375b2f8b68f9640413f08becb04f52f59e4b8f3bf2b702914"),
    ("boundary-0024/command-0017-target.bin", "91a2ef99d9afd44c3001c68b6a7b396cfb18410ed6e0c13ac2730afe5f623da9"),
    ("boundary-0058/command-0051-target.bin", "fe326c8a6a7d0b40e7958c14debe8ea1bc8d0fb160f7816bbaf9899b83590e84"),
)


def read_verified(root: Path, relative: str, expected: str) -> bytes:
    path = root / relative
    data = path.read_bytes()
    observed = hashlib.sha256(data).hexdigest()
    if observed != expected:
        raise ValueError(f"hash mismatch for {relative}: {observed}")
    if len(data) > PAGE:
        raise ValueError(f"chunk exceeds one page: {relative}")
    return data


def pack(root: Path) -> bytes:
    if root.name != CAPTURE_NAME:
        raise ValueError(f"capture directory must be named {CAPTURE_NAME}")
    image = bytearray(len(CHUNKS) * PAGE)
    for slot, (relative, digest) in enumerate(CHUNKS):
        data = read_verified(root, relative, digest)
        image[slot * PAGE : slot * PAGE + len(data)] = data
    return bytes(image)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        image = pack(args.capture)
        args.output.write_bytes(image)
    except (OSError, ValueError) as error:
        raise SystemExit(f"refusing: {error}") from error
    print(f"bytes={len(image)} sha256={hashlib.sha256(image).hexdigest()}")
    print("private_bundle=true do_not_commit=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
