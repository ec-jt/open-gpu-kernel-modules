#!/usr/bin/env python3
"""Create a small non-sparse random test file; never overwrite a path."""
import argparse
import os

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("path")
parser.add_argument("--mib", type=int, default=64)
args = parser.parse_args()
if not 1 <= args.mib <= 1024:
    parser.error("--mib must be 1..1024")
with open(args.path, "xb") as f:
    for _ in range(args.mib):
        f.write(os.urandom(1024 * 1024))
    f.flush()
    os.fsync(f.fileno())
print(f"Created {args.mib} MiB: {args.path}")
