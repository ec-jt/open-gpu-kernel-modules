# Verified GeForce GPUDirect Storage prototype

Verified on 29 October 2026 on a Linux test server with eight RTX 5090s,
patched NVIDIA 610.43.02, custom 7.0.0-rebar-iommufd-btf kernel, CUDA 13.4 /
cuFile 1.19.1, and nvidia-fs 2.31.5. Actual NVMe-to-VRAM reads pass on **all
eight GPUs**, with byte checks and matching NVFS read counters, zero POSIX
fallback reads, successful BAR1 maps, and no DMA errors.

The existing driver already supplied the BAR1 P2P modifications. No new
NVIDIA driver installation or reboot was needed for this prototype. The
custom kernel also already has the NVMe v2 GDS DMA hooks; nvidia-fs uses
`nvme_v2_register_nvfs_dma_ops`, not the older v1 interface.

## What changed

- `nv503c.cpp` captures CUDA's RM object handles and explicitly registers
  a CUDA VMM allocation with a third-party BAR1 P2P object. It uses the
  matching 610 source headers, including the changed NVOS21 ABI.
- `experimental_caps.cpp` makes cuFile attempt the registered GeForce buffer
  despite the consumer-GPU capability gate. This is an opt-in override in
  this executable only, not evidence by itself of supported hardware.
- A separate process-local mount parser shim supplies `data=ordered` only
  after `/proc/fs/ext4/<device>/options` confirms it. This custom kernel omits
  that default from `/proc/mounts`; filesystem validation remains enabled.
- `run_verified.sh` disables CPU fallback and validates actual read-path
  evidence with `verify_run.py`. A successful API call alone does not pass.

## Run on the configured server

```sh
bash /opt/geforce-gds-prototype/run_verified.sh \
  /path/on/nvme/fixture.bin 0 16 3
```

Arguments: existing fixture, GPU index, MiB per read, number of reads. The
fixture must be on supported NVMe storage. No raw-device access or writes
to the existing input file occur. `make_fixture.py` creates new random test
files exclusively and refuses to overwrite. Results live under
`~/.local/state/geforce-gds-prototype/verified.*`, or `GDS_RUN_ROOT`.

Run verification while other GDS jobs are idle: kernel counters are global.
Our wrapper serializes its own runs, and fails on unexplained counter deltas.

## Measurements and limits

- ADATA secondary NVMe → GPU 4: approximately 1.47–1.51 GB/s, three 16 MiB reads.
- Lexar system NVMe → GPU 4: approximately 3.55–3.57 GB/s, three 64 MiB reads.
- Lexar → each GPU 0–7: three 16 MiB reads each passed direct-I/O verification;
  most iterations were around 3.1–3.25 GB/s, with some slower initial reads.

These are synchronous correctness probes with reusable buffers, not peak
storage throughput or inference benchmarks. GPU-to-host copies used to
verify the bytes occur outside the timed direct read. The probe deliberately
holds host reference data for testing; production GDS need not do so.

This is **not a transparent patch for arbitrary PyTorch allocations or an
inference engine**. It handles one VMM buffer and one GPU per process.
Applications need to integrate allocation/registration and lifetime handling.
Asynchronous prefetch, concurrent buffer churn, storage writes, KV paging,
and real model execution remain unvalidated. VMM SYNC_MEMOPS is unsupported
on this stack; the prototype relies on synchronous I/O and explicit CUDA
synchronization. Do not assume asynchronous correctness from these tests.

Ordinary `gdscheck` still reports GeForce unsupported because it does not use
this executable's registration/shim. That is expected.

## Deployment: `610.43.02-gds`

This branch keeps the NVIDIA driver `src/` and `kernel-open/` trees identical
to `610.43.02-p2p` commit `14de73d818f98eba82f753132bfed8f6ed6314b7`.
It adds the userspace probe, a pinned nvidia-fs build, and post-boot verification.
It does not claim ordinary inference allocations automatically use GDS.

Prerequisites:

- Linux x86-64, matching running-kernel headers/build tools, git, Python 3,
  util-linux, systemd, root/sudo, and the NVIDIA 610.43.02 userspace driver.
- A kernel/NVMe driver with NVFS DMA hooks. Tested kernel:
  `7.0.0-rebar-iommufd-btf`. A stock kernel or another custom kernel is **not**
  validated merely by selecting this branch. Missing hooks stop the build.
- CUDA + cuFile development/runtime libraries. Tested CUDA 13.4 / cuFile
  1.19.1.55. `CUDA_ROOT` defaults to `/usr/local/cuda`; rebuilding is necessary
  after changing its target. Other cuFile versions need fresh validation.
- An NVMe-backed filesystem compatible with nvidia-fs; ext4 was tested.
  Other GDS jobs must be idle while verification checks global counters.

For a fresh deployment that already installs those prerequisites and calls
`./install.sh`, use:

```sh
git clone --branch 610.43.02-gds --single-branch \
  https://github.com/ec-jt/open-gpu-kernel-modules.git
cd open-gpu-kernel-modules
./install.sh || exit 1
# Reboot once, after the rest of your deployment has completed successfully.
```

`install.sh` builds the GDS components before installing the driver. It
excludes nvidia-peermem, as the existing deployment did: **that means Mellanox
GPUDirect RDMA is not installed by this workflow**. nvidia-fs and nvidia-peermem
serve different backends. RDMA additionally needs a compatible peer-memory
provider (or validated DMA-BUF route), NIC stack, and application registration.
The GeForce BAR1 kernel changes are retained; GDS success is not an RDMA test.

The installer backs up managed runtime/module/config files, stages the
runtime at `/opt/geforce-gds-prototype`, and enables
`geforce-gds-verify.service`. It does not unload the running GPU driver or
reboot. After the deployment's reboot, the service creates a temporary
64 MiB random file, tests each GPU sequentially, deletes that fixture, and
writes `/var/lib/geforce-gds-prototype/verified` only after every test passes.
It never formats a drive or reads/writes a raw block device. A failed test
leaves the `pending` marker and a failed service, with logs for diagnosis.
It does not reboot or restart an installer on failure.

The default fixture location is `/var/lib/geforce-gds-prototype`. If root is
not on the intended NVMe filesystem, create a directory on that filesystem
and set `GDS_FIXTURE_DIR=/path/on/nvme` in
`/etc/default/geforce-gds-prototype` before starting the service. Optional
`GDS_VERIFY_GPU=0` tests only GPU 0; otherwise all GPUs are tested.

```sh
systemctl status geforce-gds-verify.service
journalctl -u geforce-gds-verify.service -b
cat /var/lib/geforce-gds-prototype/verified
# Detailed evidence:
ls /var/lib/geforce-gds-prototype/results/verified.*/verified.json
```

On an already-running, matching patched-driver system, install only GDS:

```sh
bash tools/geforce-gds/check-build.sh
sudo bash tools/geforce-gds/stage-install.sh
sudo systemctl start geforce-gds-verify.service
```

Do not rerun an entire provisioning/cloud-init script to add GDS. In existing
scripts, replace both the clone URL and checkout branch; fail explicitly if
`./install.sh` fails, even if an outer script uses `set +e`. Make unrelated
`nvidia-patch` clone steps idempotent and remove one-shot provisioning markers
before the final reboot, so a successful provision does not loop. A branch
change cannot fix failures in the caller's other steps. The included
[`deployment-change.patch`](deployment-change.patch) contains the minimal
edits for the existing `install-p2p-kernel.sh` layout (including its Ubuntu
one-shot marker). Apply those edits to the script embedded in your deployment
template; do not rerun the whole template on an already-provisioned machine.

Pinned nvidia-fs revision:
`6a65099b667e1838109952a87dcc6c9414ebf81f` (2.31.5).
Use the NVIDIA source tree supplied by this checkout when building; ABI
checks reject changed driver source. The source checkout must be clean in
`src/` and `kernel-open/`.

To disable verification/boot loading, disable `geforce-gds-verify.service`
and remove the two `geforce-gds-prototype.conf` files under
`/etc/modules-load.d` and `/etc/modprobe.d`. Stop all GDS applications before
unloading `nvidia_fs`. Restore managed files from the installer's backup if
necessary. The installer is not transactional across a power failure.

## References and history

The NV503C approach follows the mechanism in
[mcornea/geforce-gpudirect-rdma](https://github.com/mcornea/geforce-gpudirect-rdma)
and the [RTX 3060 userspace GDS report](https://wanger-sjtu.github.io/2026-09-09-geforce-gds-userspace-nv503c-hook/),
using NVIDIA's version-matched ABI rather than the old handwritten structures.
NVIDIA kernel headers retain their own licenses. Only the prototype's own
source is included here; external reference patches are not bundled.
