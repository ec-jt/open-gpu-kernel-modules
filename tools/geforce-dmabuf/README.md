# Experimental GeForce DMA-BUF RDMA

On the tested patched 610.43.02 driver, a CUDA VMM allocation can be exported
through NVIDIA's existing kernel DMA-BUF ioctl and registered with mlx5.
No new kernel patch, nvidia-peermem, or NV503C registration was required.
The normal CUDA export API still returns `CUDA_ERROR_NOT_SUPPORTED`.

## Verified result

Tested on eight RTX 5090 GPUs with kernel `7.0.0-rebar-iommufd-btf`:

- DMA-BUF export and `ibv_reg_dmabuf_mr` registration passed for every GPU
  against all four mlx5 devices (32 GPU/NIC registrations).
- The active ConnectX-5 and ConnectX-4 Lx ports each completed a 2 MiB RDMA
  WRITE into GPU memory and a 2 MiB RDMA READ from GPU memory for every GPU.
  All byte comparisons passed: 16 GPU/NIC pairs, each tested in both directions.
- The other two ports were inactive: registration passed, transfer tests
  explicitly skipped them.
- These tests used two RC queue pairs on the same NIC. **Inter-node RDMA,
  network bandwidth/latency, NCCL, and inference integration are untested.**

## How it works

1. Create and map device memory using CUDA VMM (`cuMemCreate`/`cuMemMap`).
2. Reuse the GDS prototype's ioctl capture to obtain the allocation's RM
   client and memory handles. Handle capture alone does not register NV503C.
3. Call `NV_ESC_EXPORT_TO_DMABUF_FD` on the matching GPU device node with
   the captured handles, default mapping type, and no CPU mmap request.
4. Register the returned FD using `ibv_reg_dmabuf_mr`.
5. Post real RDMA READ/WRITE work requests and wait for NIC completion.

For this non-coherent PCIe platform the existing kernel exporter uses BAR1.
Its normal validation and resource lifetime handling are retained. The
prototype does not spoof the CUDA DMA-BUF capability or treat an overridden
capability as success. `--use_cuda_dmabuf` in ordinary perftest still calls
the rejected CUDA export API; it does not automatically use this workaround.

The previous peermem approach creates an NV503C object and uses persistent
P2P pinning. Here, the existing DMA-BUF exporter already performs its own
memory-handle duplication and BAR mapping. We reuse the allocation/capture
approach, not the peermem-specific patch. See the
[original GeForce RDMA work](https://github.com/mcornea/geforce-gpudirect-rdma).

## Build and run

Requires this driver's source headers, CUDA development libraries,
libibverbs development headers/providers, and an active mlx5 port for the
transfer test. The program requires loaded NVIDIA driver 610.43.02.

```sh
cd tools/geforce-dmabuf
make
./dmabuf_probe 0
# Optional comparison: also create NV503C before export.
./dmabuf_probe 0 nv503c
```

The default is **without NV503C registration**. It allocates one small VMM
buffer, registers it on each available verbs device in turn, and tests local
NIC loopback on active port 1. A complete successful run ends with:

```text
VERIFIED registrations=4 local_rdma_pairs=2 inter_node_tested=false
```

No successful active-port transfer means failure, even if registration
succeeded. Results depend on the available NICs and their port states.
The probe writes only its own newly allocated GPU and host test buffers.
It never accesses another application's allocation, disk data, or a remote
machine. No nvidia-fs calls are involved in these RDMA transfers.

## Integrity and scope

For WRITE, host test bytes are transferred into a GPU buffer previously
poisoned with a different value, then copied back through CUDA for comparison.
For READ, a different pattern is placed in VRAM, the host destination is
poisoned, and the NIC reads GPU memory into it for comparison. CUDA copies
prepare/check data; they do not perform the tested RDMA operation.

This is a synchronous, single-buffer, single-thread experiment tied to the
610 RM ABI. It uses an internal NVIDIA ioctl with official version-matched
headers. The full CUDA allocation lifetime must outlast the DMA-BUF and all
NIC memory registrations; deregister before closing the FD or releasing VRAM.
The test does not establish asynchronous inference correctness, memory-pool
support, arbitrary PyTorch allocation export, or support across driver versions.

The GDS deployment installer does not silently install this experiment into
applications or replace system libcuda/libibverbs. Application integration
must explicitly use the exporter and maintain those lifetimes. No reboot is
needed to build and run this prototype on the tested existing driver.
