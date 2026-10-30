// Version-pinned experimental export of the allocation created by this process.
#include "nv503c.hpp"
#include <nv-ioctl.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cerrno>
#include <stdexcept>

int export_captured_dmabuf(const char* device_path,std::size_t bytes) {
    auto mem=nv503c_captured_memory();
    int device_fd=open(device_path,O_RDWR|O_CLOEXEC);
    if(device_fd<0) throw std::runtime_error("Cannot open GPU device node");
    nv_ioctl_export_to_dma_buf_fd_t e{};
    e.fd=-1; e.hClient=mem.client; e.totalObjects=1; e.numObjects=1;
    e.totalSize=bytes; e.mappingType=NV_DMABUF_EXPORT_MAPPING_TYPE_DEFAULT;
    e.bAllowMmap=0; e.handles[0]=mem.memory; e.sizes[0]=bytes;
    int rc=ioctl(device_fd,_IOWR('F',NV_ESC_EXPORT_TO_DMABUF_FD,nv_ioctl_export_to_dma_buf_fd_t),&e);
    int saved=rc<0?errno:0;
    close(device_fd);
    fprintf(stderr,"DIRECT_DMABUF_EXPORT ioctl=%d errno=%d RM_status=0x%x fd=%d bytes=%zu\n",rc,saved,e.status,e.fd,bytes);
    if(rc<0 || e.status || e.fd<0) throw std::runtime_error("Direct DMA-BUF export failed");
    return e.fd;
}
