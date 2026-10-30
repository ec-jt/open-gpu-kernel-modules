# Opt-in shim for the ordinary CUDA DMA-BUF export API

This prototype makes `cuMemGetHandleForAddressRange(...,
CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0)` work for a restricted class of
allocations on the tested GeForce 615.78.08 stack. It is a process-local
`LD_PRELOAD` library; it does not replace libcuda or change the CUDA capability
attributes system-wide. Without this preload the same client returns
`CUDA_ERROR_NOT_SUPPORTED`.

## Verified

On eight RTX 5090s, each GPU passed all four lookup routes: directly linked
CUDA calls, `dlsym`, `cuGetProcAddress`, and `cuGetProcAddress_v2`. For every
route, two simultaneously live allocations were exported and registered
with mlx5. Both active NICs performed real RDMA WRITE and READ with byte
checks. That is 64 exported buffers and 128 GPU-buffer/NIC transfer pairs
across the eight GPUs. Two other inactive NIC ports passed registration
and skipped transfers. There were also 136 expected rejections (17/GPU).

The client in `api-probe.cpp` uses public CUDA APIs and libibverbs only. It
does not call the direct exporter or manually capture driver handles.
This establishes that the API shim works, not just that our old helper works.

## Build and run

```sh
cd tools/geforce-dmabuf/shim
make
# Native control: expected to fail export with CUDA_ERROR_NOT_SUPPORTED.
./api-probe 0
# Opt-in shim: real local NIC read/write integrity tests.
LD_PRELOAD="$PWD/libgeforce-dmabuf-shim.so" ./api-probe 0
```

Use only with the reviewed driver and a dedicated test process. No global
`/etc/ld.so.preload` change is needed or recommended. Do not combine it with
the separate GDS executable's ioctl/dlsym interceptors. The branch installer
does not preload this into vLLM, PyTorch, NCCL or other applications.

## Supported contract

- Linux x86-64, NVIDIA 615.78.08, CUDA VMM pinned device allocations created
  after loading the shim, with no requested external handle types.
- One complete physical allocation mapped at offset zero, exported using
  exactly the mapped base address and full allocation size, with flags zero.
- Multiple live allocations, each tracked separately. Capture is thread-local;
  the tracking operations are serialized, but concurrent application workloads
  have not been validated.
- Retain the CUDA allocation and mapping until all NIC work completes, then
  deregister MRs and close exported FDs before unmapping/releasing memory.
- Native successful exports are returned unchanged. The fallback only runs
  when CUDA returns `CUDA_ERROR_NOT_SUPPORTED` and the allocation is known.

Partial ranges, early-released handles, unmapped addresses, ordinary
`cuMemAlloc` buffers, async memory pools, imported allocations, arrays,
manually constructed RM objects and nonzero export flags are outside this
fallback. These are rejected or left to the real CUDA implementation; the
shim never guesses which allocation an arbitrary pointer belongs to.

The shim hooks the CUDA allocation/mapping/release APIs and pointer lookup
APIs, then captures the matching RM memory object during `cuMemCreate`. Its
export fallback calls the existing kernel DMA-BUF exporter. It does not use
nvidia-peermem, NV503C registration, or bypass the kernel's importer checks.

## Remaining work

Unmodified applications may query the unsupported capability first, use
shareable VMM handles, allocate through `cuMemAlloc` or pools, or request
subranges. Those paths need explicit support and validation; this is not yet
a universal perftest/NCCL/PyTorch compatibility layer. Inter-node transfer,
application integration, throughput, concurrent lifetime races and fork/context
teardown are not validated. The ioctl interposer is scoped to these CUDA/verbs
clients, not a general interceptor for arbitrary programs.
