# Experimental GeForce GDS branch: 615.78.08-gds

This branch preserves the P2P driver below and adds a verified NVMe-to-VRAM
prototype. **Read [the GDS setup and limitations](tools/geforce-gds/README.md)
before installation.** `./install.sh` now stages GDS and schedules verification
at the next boot. It requires NVMe NVFS kernel hooks and CUDA/cuFile development
libraries; it does not turn arbitrary applications into GDS users or install
Mellanox peer-memory support.

The original P2P documentation, configuration notes, and upstream benchmarks
are preserved in [README-p2p.md](README-p2p.md).

## Deployment

Use `ec-jt/open-gpu-kernel-modules`, branch `615.78.08-gds`, and the
[deployment patch](tools/geforce-gds/deployment-change.patch). The install
hook builds and stages the prototype; after reboot,
`geforce-gds-verify.service` checks direct reads on every GPU. A branch change
alone is insufficient: install matching **615.78.08 NVIDIA userspace** too.
The installer checks that version and applies upstream's backed-up libcuda
P2P patch. NVMe kernel hooks and CUDA/cuFile packages are also required.

## Validation

On the tested eight-RTX-5090 system, the packaged build and verification
service passed on all GPUs with byte-verified reads, matching NVFS counters,
and zero POSIX fallback. The server then rebooted and the automatic all-GPU GDS verification passed.
All 56 ordered GPU peer-copy pairs passed integrity checks before and after
reboot. A fresh full cloud-init provisioning cycle has not been tested.

The prototype supports one synchronous VMM buffer and one GPU per process.
A separate [DMA-BUF RDMA prototype](tools/geforce-dmabuf/README.md) now passes
local hardware RDMA READ/WRITE integrity tests on all eight GPUs through the
two active mlx5 adapters, without nvidia-peermem or NV503C registration.
Inter-node testing and application/inference integration remain separate
work; see each prototype README for scope and version pins.

## Ordinary CUDA export shim and port audit

An opt-in [CUDA API shim](tools/geforce-dmabuf/shim/README.md) now makes the
ordinary DMA-BUF export API work for tracked, whole VMM allocations. All eight
GPUs passed real local RDMA integrity checks through four API lookup routes.
It is not a universal CUDA/PyTorch/NCCL compatibility layer.

The [port audit](tools/geforce-gds/PORT-AUDIT.md) records the retained P2P,
hugepage, GDS and RDMA work, plus the separate older diagnostic branch and
the importer bypass that was deliberately not carried forward.
