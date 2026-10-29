#!/usr/bin/env bash
# Check ABI and build both components before changing the installed driver.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(cd "$here/../.." && pwd)
export NVIDIA_SOURCE="$source_dir"
[ "$(uname -s)" = Linux ] || { echo 'Linux required' >&2; exit 1; }
# Pin the actual driver trees, not the branch commit (which changes as tools are added).
[ "$(git -C "$source_dir" rev-parse HEAD:src)" = 9c2818d6dd203329e822896ada1409ccf581c780 ]
[ "$(git -C "$source_dir" rev-parse HEAD:kernel-open)" = 41a9393eae0b0aa3e1b4557dc484c9cb42b21106 ]
git -C "$source_dir" diff --quiet HEAD -- src kernel-open
# The old deployment temporarily moves nvidia-peermem out of this tree. That
# workaround is unnecessary for this branch; keep the source intact for validation.
test -r /proc/kallsyms
if ! grep -Eq '[[:space:]][A-Za-z][[:space:]]nvme_(v2_)?register_nvfs_dma_ops([[:space:]]|$)' /proc/kallsyms; then
    echo 'NVMe nvidia-fs DMA hooks are absent. This kernel is not ready for the validated NVFS backend.' >&2
    exit 1
fi
cuda=${CUDA_ROOT:-/usr/local/cuda}
test -r "$cuda/include/cufile.h" || { echo 'Install matching CUDA/cuFile development packages first' >&2; exit 1; }
make -C "$here" CUDA_ROOT="$cuda"
bash "$here/build-nvfs.sh"
