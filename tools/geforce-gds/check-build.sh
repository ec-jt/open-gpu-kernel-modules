#!/usr/bin/env bash
# Check ABI and build both components before changing the installed driver.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(cd "$here/../.." && pwd)
export NVIDIA_SOURCE="$source_dir"
[ "$(uname -s)" = Linux ] || { echo 'Linux required' >&2; exit 1; }
# Pin the actual driver trees, not the branch commit (which changes as tools are added).
[ "$(git -C "$source_dir" rev-parse HEAD:src)" = d5e553e54761373accd8b27e3895ae34f4d365ac ]
[ "$(git -C "$source_dir" rev-parse HEAD:kernel-open)" = a3b7a2bc679f44b0c7e66e7964c2fdc3db5503de ]
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
