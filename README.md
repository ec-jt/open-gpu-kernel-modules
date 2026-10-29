# Experimental GeForce GDS branch: 610.43.02-gds

This branch preserves the P2P driver below and adds a verified NVMe-to-VRAM
prototype. **Read [the GDS setup and limitations](tools/geforce-gds/README.md)
before installation.** `./install.sh` now stages GDS and schedules verification
at the next boot. It requires NVMe NVFS kernel hooks and CUDA/cuFile development
libraries; it does not turn arbitrary applications into GDS users or install
Mellanox peer-memory support.

The original P2P documentation, configuration notes, and upstream benchmarks
are preserved in [README-p2p.md](README-p2p.md).

## Deployment

Use `ec-jt/open-gpu-kernel-modules`, branch `610.43.02-gds`, and the
[deployment patch](tools/geforce-gds/deployment-change.patch). The install
hook builds and stages the prototype; after reboot,
`geforce-gds-verify.service` checks direct reads on every GPU. A branch change
alone cannot supply missing NVMe kernel hooks or CUDA/cuFile packages.

## Validation

On the tested eight-RTX-5090 system, the packaged build and verification
service passed on all GPUs with byte-verified reads, matching NVFS counters,
and zero POSIX fallback. The service was manually started on the running
patched driver; a fresh provisioning/reboot cycle has not been tested.

The prototype supports one synchronous VMM buffer and one GPU per process.
A separate [DMA-BUF RDMA prototype](tools/geforce-dmabuf/README.md) now passes
local hardware RDMA READ/WRITE integrity tests on all eight GPUs through the
two active mlx5 adapters, without nvidia-peermem or NV503C registration.
Inter-node testing and application/inference integration remain separate
work; see each prototype README for scope and version pins.
