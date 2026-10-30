# 610 to 615 port audit

The branch starts at upstream `615.78.08-p2p`
`efed6748b6e7bfc81273f61c148554aa5944e107`. All eight upstream commits from
the earlier comparison are therefore ancestors of this branch.

| Earlier feature | 615 disposition |
| --- | --- |
| 610 combined BAR1 P2P/NVLink patch, `3590ded0` | New upstream P2P implementation `682690e1`; includes updated Blackwell UVM handling and mixed-generation mapping. |
| Hugepage host registration, `565e1b53` | New upstream implementation `0b1b3902`; includes additional mapping/suballocation handling. No new speed claim was measured here. |
| P2P README, `14de73d8` | New upstream documentation preserved verbatim in `README-p2p.md`. |
| GDS prototype, `0ee50a4` | Ported tools, version/source-tree pins, nvidia-fs build, verification service and deployment changes. |
| DMA-BUF notes, `ec381bd` | Preserved distinction between unsupported CUDA export and working kernel export. |
| Direct DMA-BUF exporter, `ee50771` | Ported and verified on all eight GPUs. |
| 615 libcuda P2P patch, `610890ff` | Included and applied via a version-checked, backed-up, repeat-safe wrapper. |

The driver `src/` and `kernel-open/` trees remain exactly upstream's 615 P2P
trees. The old 610 GDS branch remains available. Runtime checks establish
same-generation 5090 P2P, direct GDS and local NIC DMA-BUF transfers; they do
not validate mixed-generation GPUs, NVLink or the hugepage speed claim.

## Additional branch discovered on the second server

Its source checkout was `610.43.02-p2p-dma`, commit
`7bd763ba8ed118de7a25e6133363188efc138ea8`, rather than the plain
`610.43.02-p2p` branch. That separate branch adds:

- `9db63320`: DMA-BUF/peermem diagnostic counters.
- `5c7dbab4`: excludes nvidia-peermem when MLNX-OFED DKMS is present. Our
  installer already excludes nvidia-peermem for this DMA-BUF-based setup.
- `7bd763ba`: extra attach/topology tracing and removal of an importer check
  for MMIO memory without struct-page backing.

The counters and logging are not included in the new driver source. The
importer-check bypass is also not carried forward: the tested mlx5 DMA-BUF
path works with the upstream checks retained. Its absence is deliberate,
not an assertion that all arbitrary importers are supported. The old source
checkout and installed-driver rollback were preserved before upgrading.

The new optional CUDA API shim is separate userspace code under
`tools/geforce-dmabuf/shim`; it also keeps those kernel checks intact.
