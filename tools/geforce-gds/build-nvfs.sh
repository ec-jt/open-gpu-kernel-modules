#!/usr/bin/env bash
# Build only: no module installation, loading, driver replacement or reboot.
set -euo pipefail
probe_dir=$(cd -- "$(dirname -- "$0")" && pwd)
nvfs_revision=6a65099b667e1838109952a87dcc6c9414ebf81f
nv_source=${NVIDIA_SOURCE:-$(cd "$probe_dir/../.." && pwd)}
nvfs_source="$probe_dir/vendor-nvidia-fs"
test -f "$nv_source/kernel-open/nvidia/nv-p2p.h" || {
  echo "Set NVIDIA_SOURCE to the matching open GPU module source tree" >&2
  exit 1
}
test -d "/lib/modules/$(uname -r)/build" || {
  echo "Headers for the running kernel are required" >&2
  exit 1
}
if [ ! -d "$nvfs_source" ]; then
  git clone https://github.com/NVIDIA/gds-nvidia-fs.git "$nvfs_source"
  git -C "$nvfs_source" checkout --detach "$nvfs_revision"
fi
test "$(git -C "$nvfs_source" rev-parse HEAD)" = "$nvfs_revision" || {
  echo "Source revision differs from the reviewed build; refusing to reset it" >&2
  exit 1
}
git -C "$nvfs_source" diff --quiet --exit-code || {
  echo "Tracked GDS source changes present; review before building" >&2
  exit 1
}
(cd "$nvfs_source/src" && make NVIDIA_SRC_DIR="$nv_source/kernel-open/nvidia")
modinfo "$nvfs_source/src/nvidia-fs.ko"
echo "Build complete only. Module has NOT been installed or loaded."
