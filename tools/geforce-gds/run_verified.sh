#!/usr/bin/env bash
set -euo pipefail
if [ "$#" -lt 1 ] || [ "$#" -gt 4 ]; then
    echo 'Usage: bash run_verified.sh EXISTING_FIXTURE [GPU=0] [MiB=16] [ITERATIONS=3]' >&2
    exit 2
fi
here=$(cd -- "$(dirname -- "$0")" && pwd)
fixture=$(realpath -e -- "$1")
gpu=${2:-0}
mib=${3:-16}
iterations=${4:-3}
# Global NVFS deltas require an otherwise idle GDS workload. Serialize our own tests.
runroot=${GDS_RUN_ROOT:-${XDG_STATE_HOME:-$HOME/.local/state}/geforce-gds-prototype}
mkdir -p -- "$runroot"
exec 9>"$runroot/.verification.lock"
flock -n 9 || { echo 'Another verification is running' >&2; exit 1; }
[ -r /proc/driver/nvidia-fs/stats ] || { echo 'Load nvidia-fs with rw_stats_enabled=1 peer_stats_enabled=1 first' >&2; exit 1; }
log=$(mktemp -d "$runroot/verified.XXXXXX")
cat /proc/driver/nvidia-fs/stats > "$log/nvfs.before.txt"
export CUFILE_ENV_PATH_JSON="$here/strict.json"
export CUFILE_ALLOW_COMPAT_MODE=false CUFILE_FORCE_COMPAT_MODE=false CUFILE_STATS_LEVEL=3
export GDS_EXPERIMENTAL_CAPABILITY_OVERRIDE=1 GDS_NORMALIZE_EXT4_OPTIONS=1
cd "$log"
set +e
"$here/gds_probe" "$fixture" gds "$gpu" "$mib" "$iterations" nv503c > output.txt 2>&1
status=$?
set -e
cat /proc/driver/nvidia-fs/stats > nvfs.after.txt
cat output.txt
printf 'Logs: %s\n' "$log"
[ "$status" -eq 0 ] || exit "$status"
python3 "$here/verify_run.py" "$log" --gpu "$gpu" --mib "$mib" --iterations "$iterations" | tee verified.json
