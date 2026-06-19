#!/bin/bash
# setup-doca.sh — Install DOCA / MLNX-OFED packages for Mellanox NICs.
#
# Idempotent: safe to re-run. Skips already-installed packages.
# Called by install.sh when a Mellanox NIC is detected and doca-ofed is missing.
#
# Packages provide:
#   • GPUDirect RDMA (nvidia-peermem via DOCA, not open-gpu-kernel-modules)
#   • RDMA verbs (libibverbs, librdmacm, ibverbs-providers)
#   • Firmware tools (mft, mstflint)
#   • OFED kernel modules (mlnx-rdma-dkms, mlnx-nvme-dkms)
#
# Usage:
#   sudo ./setup-doca.sh
#   DOCA_VERSION=3.2.0 sudo ./setup-doca.sh

set -euo pipefail

DOCA_VERSION="${DOCA_VERSION:-3.2.0}"
DOCA_DISTRO="${DOCA_DISTRO:-ubuntu22.04}"

# Prefer apt-fast if available
APT="apt-get"
command -v apt-fast &>/dev/null && APT="apt-fast"

echo ">>> Setting up DOCA ${DOCA_VERSION} / MLNX-OFED for Mellanox NIC"

# ── Add DOCA repo if missing ─────────────────────────────────────────
if [ ! -f /etc/apt/sources.list.d/doca.list ]; then
  echo ">>> Adding DOCA ${DOCA_VERSION} repository..."
  curl -fsSL https://linux.mellanox.com/public/repo/doca/GPG-KEY-Mellanox.pub \
    | gpg --dearmor -o /etc/apt/trusted.gpg.d/GPG-KEY-Mellanox.pub
  echo "deb [signed-by=/etc/apt/trusted.gpg.d/GPG-KEY-Mellanox.pub] https://linux.mellanox.com/public/repo/doca/${DOCA_VERSION}/${DOCA_DISTRO}/x86_64 ./ " \
    | tee /etc/apt/sources.list.d/doca.list > /dev/null
  $APT update
fi

# ── Install packages (idempotent — apt skips already-installed) ──────
echo ">>> Installing DOCA / MLNX-OFED packages..."
$APT install -y \
  doca-ofed doca-extra \
  mft mft-mlx5 mstflint \
  mlnx-rdma-dkms mlnx-nvme-dkms fwctl-dkms \
  rdma-core librdmacm-dev libibverbs-dev ibverbs-providers \
  libibverbs1 librdmacm1 libpciaccess-dev libnuma-dev libnl-3-dev libnl-route-3-dev

# ── Apply tuning ─────────────────────────────────────────────────────
echo ">>> Applying Mellanox tuning..."
mst start 2>/dev/null || true
mlnx_tune -p HIGH_THROUGHPUT 2>/dev/null || true

# ── Verify ────────────────────────────────────────────────────────────
if command -v /opt/mellanox/doca/tools/doca-info &>/dev/null; then
  /opt/mellanox/doca/tools/doca-info 2>/dev/null || true
fi
ibv_devinfo 2>/dev/null || true

echo ">>> DOCA / MLNX-OFED setup complete"
