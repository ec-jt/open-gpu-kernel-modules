#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
# Compatibility with the deployment's old directory-move workaround. Exclude
# peermem using Kbuild instead, leaving the reviewed driver source intact.
if [ -d kernel-open/nvidia-peermem.disabled ] && [ ! -e kernel-open/nvidia-peermem ]; then
    mv kernel-open/nvidia-peermem.disabled kernel-open/nvidia-peermem
fi
bash tools/geforce-gds/check-build.sh
# NVMe GDS uses nvidia-fs, not nvidia-peermem. mlx RDMA requires a separate
# kernel/provider-compatible peermem build or a validated DMA-BUF path.
make modules -j"$(nproc)" NV_EXCLUDE_KERNEL_MODULES=nvidia-peermem
sudo make modules_install -j"$(nproc)" NV_EXCLUDE_KERNEL_MODULES=nvidia-peermem
sudo depmod -a "$(uname -r)"
sudo bash tools/geforce-gds/stage-install.sh
echo 'Installation staged. Reboot to load the driver and run GDS verification.'
echo 'No NVIDIA module was unloaded by this installer; active jobs were not stopped.'
