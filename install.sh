#!/bin/bash
# Exclude nvidia-peermem when MLNX-OFED DKMS is present — its IB modules
# clash with vmlinux built-in InfiniBand symbols during modpost.
# DOCA/OFED provides its own nvidia-peermem; no functionality lost.
set -e

EXCLUDE=""
if [ -d /var/lib/dkms/mlnx-ofed-kernel ]; then
  echo ">>> MLNX-OFED DKMS detected — excluding nvidia-peermem"
  EXCLUDE="nvidia-peermem"
fi

sudo rmmod nvidia_drm nvidia_modeset nvidia_uvm nvidia 2>/dev/null || true
make modules -j$(nproc) NV_EXCLUDE_KERNEL_MODULES="$EXCLUDE"
sudo make modules_install -j$(nproc) NV_EXCLUDE_KERNEL_MODULES="$EXCLUDE"
sudo depmod
nvidia-smi
