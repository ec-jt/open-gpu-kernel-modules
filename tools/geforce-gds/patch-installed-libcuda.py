#!/usr/bin/env python3
"""Apply upstream's version-specific P2P patch with a backup and repeat guard."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

VERSION = "615.78.08"
source = Path(__file__).resolve().parents[2] / "patch-libcuda-p2p.py"
library = Path(sys.argv[1] if len(sys.argv) > 1 else
               "/usr/lib/x86_64-linux-gnu/libcuda.so.1").resolve(strict=True)
if library.name != "libcuda.so." + VERSION:
    raise SystemExit("Install matching NVIDIA 615.78.08 userspace first: " + str(library))
if len(sys.argv) > 2 and sys.argv[2] == "--check-version":
    print("Matched userspace library: " + str(library))
    raise SystemExit(0)
if os.geteuid() != 0:
    raise SystemExit("Run as root")
state = Path("/var/lib/geforce-gds-prototype/libcuda-p2p")
state.mkdir(parents=True, exist_ok=True, mode=0o700)
with (state / "lock").open("a") as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    digest = hashlib.sha256(library.read_bytes()).hexdigest()
    marker = state / (VERSION + ".json")
    if marker.exists():
        previous = json.loads(marker.read_text())
        if previous["library"] == str(library) and previous["patched_sha256"] == digest:
            print("Upstream libcuda P2P patch already applied and hash verified")
            raise SystemExit(0)
    backup = state / (library.name + "." + digest + ".original")
    if not backup.exists():
        shutil.copy2(library, backup)
    if hashlib.sha256(backup.read_bytes()).hexdigest() != digest:
        raise SystemExit("Backup hash mismatch; refusing to patch")
    fd, temporary = tempfile.mkstemp(prefix=".libcuda-p2p-", dir=library.parent)
    os.close(fd)
    try:
        shutil.copy2(library, temporary)
        subprocess.run([sys.executable, str(source), temporary], check=True)
        patched = hashlib.sha256(Path(temporary).read_bytes()).hexdigest()
        with open(temporary, "rb") as result:
            os.fsync(result.fileno())
        os.replace(temporary, library)
        record = dict(library=str(library), backup=str(backup),
                      original_sha256=digest, patched_sha256=patched)
        marker.write_text(json.dumps(record, indent=2) + "\n")
        print("Patched libcuda; original backed up at " + str(backup))
    finally:
        Path(temporary).unlink(missing_ok=True)
