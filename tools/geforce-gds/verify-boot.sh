#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
state=/var/lib/geforce-gds-prototype
mkdir -p "$state"
rm -f "$state/verified"
[ "$(cat /sys/module/nvidia/version)" = 610.43.02 ] || { echo 'Loaded NVIDIA driver must be 610.43.02' >&2; exit 1; }
modprobe nvidia_uvm
modprobe nvidia_fs rw_stats_enabled=1 peer_stats_enabled=1
printf 1 > /sys/module/nvidia_fs/parameters/rw_stats_enabled
printf 1 > /sys/module/nvidia_fs/parameters/peer_stats_enabled
# A private directory, never a raw device; exclusively create a fresh fixture.
fixture_root=${GDS_FIXTURE_DIR:-$state}
test -d "$fixture_root"
fixture_dir=$(mktemp -d "$fixture_root/fixture.XXXXXX")
trap 'rm -f -- "$fixture_dir/test.bin"; rmdir -- "$fixture_dir"' EXIT
python3 "$here/make_fixture.py" "$fixture_dir/test.bin" --mib 64
export GDS_RUN_ROOT="$state/results"
# Sequential probes on all GPUs by default. Override with a single integer index.
gpu_list=${GDS_VERIFY_GPU:-$(nvidia-smi --query-gpu=index --format=csv,noheader)}
test -n "$gpu_list"
for gpu in $gpu_list; do
    [[ "$gpu" =~ ^[0-9]+$ ]] || { echo 'Invalid GPU index' >&2; exit 1; }
    bash "$here/run_verified.sh" "$fixture_dir/test.bin" "$gpu" 16 3
done
date -u +%FT%TZ > "$state/verified"
rm -f "$state/pending"
echo 'Direct NVFS reads verified, with byte checks and zero POSIX fallback.'
