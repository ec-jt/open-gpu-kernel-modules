#!/usr/bin/env python3
"""Fail-closed check of this probe's integrity output and actual cuFile/NVFS IO."""
import argparse
import json
from pathlib import Path
import re
import sys


def verify(path, gpu, mib, iterations):
    path = Path(path)
    output = (path / 'output.txt').read_text()
    log = (path / 'cufile.log').read_text()
    before = (path / 'nvfs.before.txt').read_text()
    after = (path / 'nvfs.after.txt').read_text()
    expected = mib * iterations
    rows = re.findall(r'mode=gds allocator=nv503c iteration=(\d+) bytes=(\d+) seconds=([\d.e+-]+) GB_per_s=([\d.e+-]+) data_verified=true', output)
    if len(rows) != iterations or [int(x[0]) for x in rows] != list(range(iterations)):
        raise ValueError('Missing successful, sequential byte-verified reads')
    if any(int(x[1]) != mib * 1024 * 1024 for x in rows):
        raise ValueError('Wrong transfer size')
    if 'FAIL:' in output or 'NV503C cleanup:' in output:
        raise ValueError('Probe or registration cleanup failed')
    summaries = re.findall(r'^GPU ' + str(gpu) + r'\(UUID:.*?\) Read:(.*?) Write:', log, re.M)
    if len(summaries) != 1:
        raise ValueError('Missing or ambiguous cuFile per-GPU summary')
    s = summaries[0]
    def field(name):
        match = re.search(r'\b' + re.escape(name) + r'=(\d+)\b', s)
        if not match:
            raise ValueError('Missing cuFile field: ' + name)
        return int(match[1])
    n, nvfs, posix, errors, read_mib = [field(x) for x in ('n', 'nvfs', 'posix', 'err', 'MiB')]
    if n <= 0 or n != nvfs or posix != 0 or errors != 0 or read_mib != expected:
        raise ValueError('cuFile direct-read evidence does not match expected bytes, or fallback/errors occurred')
    for name in ('pcie p2pdma', 'unalign', 'dr', 'r_sparse', 'r_inline'):
        if field(name):
            raise ValueError('Unexpected alternate read path: ' + name)
    def counters(text):
        read = re.search(r'^Reads\s*: n=(\d+) ok=(\d+) err=(\d+) readMiB=(\d+) io_state_err=(\d+)', text, re.M)
        bar = re.search(r'^Bar1-map\s*: n=(\d+) ok=(\d+) err=(\d+) free=(\d+) callbacks=(\d+) active=(\d+)', text, re.M)
        dma = re.search(r'^Error\s*: cpu-gpu-pages=(\d+) sg-ext=(\d+) dma-map=(\d+) dma-ref=(\d+)', text, re.M)
        if not all((read, bar, dma)):
            raise ValueError('Missing enabled NVFS counters')
        return [list(map(int, m.groups())) for m in (read, bar, dma)]
    b, a = counters(before), counters(after)
    rd = [x-y for x,y in zip(a[0], b[0])]
    if rd[0] != nvfs or rd[1] != nvfs or rd[2] or rd[3] != expected or rd[4]:
        raise ValueError('NVFS read-counter deltas mismatch; possible concurrent workload or I/O failure')
    if a[1][1] <= b[1][1] or a[1][2] != b[1][2] or a[1][5] != b[1][5] or a[2] != b[2]:
        raise ValueError('BAR1 mapping/cleanup or DMA error counters failed')
    if not re.search(r'compat: 0 size:', log) or 'bounce-buffer ptr 0' not in log:
        raise ValueError('Missing direct submission traces')
    result = dict(verified=True, gpu=gpu, backend='nvidia-fs', mib=expected,
                  nvfs_reads=nvfs, posix_reads=posix, data_verified=True,
                  GB_per_s=[float(x[3]) for x in rows], log_directory=str(path.resolve()))
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run_directory')
    ap.add_argument('--gpu', type=int, required=True)
    ap.add_argument('--mib', type=int, required=True)
    ap.add_argument('--iterations', type=int, required=True)
    args = ap.parse_args()
    try:
        result = verify(args.run_directory, args.gpu, args.mib, args.iterations)
        print(json.dumps(result, indent=2))
        return 0
    except (ValueError, OSError) as e:
        print('DIRECT GDS NOT VERIFIED: ' + str(e), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
