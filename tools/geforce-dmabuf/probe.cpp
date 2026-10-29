#include "nv503c.hpp"
#include <cuda.h>
#include <infiniband/verbs.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <string>
#include <stdexcept>
int export_captured_dmabuf(const char*,std::size_t);
bool verify_local_rdma(ibv_context*,ibv_pd*,ibv_mr*,CUdeviceptr,std::size_t);
static void check(CUresult r,const char* stage) {
    if(r) { const char* name=nullptr; cuGetErrorName(r,&name); throw std::runtime_error(std::string(stage)+": "+(name?name:"unknown")); }
}
struct Buffer {
    CUdeviceptr va=0; CUmemGenericAllocationHandle mem=0; size_t size=0;
    bool mapped=false; int fd=-1;
    ~Buffer() {
        if(fd>=0) close(fd);
        nv503c_unregister();
        if(mapped) cuMemUnmap(va,size);
        if(mem) cuMemRelease(mem);
        if(va) cuMemAddressFree(va,size);
    }
};
int main(int argc,char**argv) {
    try {
        std::ifstream version_file("/sys/module/nvidia/version"); std::string version;
        std::getline(version_file,version);
        if(version!="610.43.02") throw std::runtime_error("This prototype requires loaded driver 610.43.02");
        if(argc>3 || (argc>2 && std::string(argv[2])!="nv503c")) throw std::runtime_error("Usage: dmabuf_probe [GPU=0] [nv503c]");
        int gpu=argc>1?std::stoi(argv[1]):0;
        if(gpu<0 || gpu>255) throw std::runtime_error("GPU index out of range");
        bool register_p2p=argc>2 && std::string(argv[2])=="nv503c";
        check(cuInit(0),"init"); CUdevice dev; check(cuDeviceGet(&dev,gpu),"device");
        CUcontext ctx; check(cuDevicePrimaryCtxRetain(&ctx,dev),"context");
        check(cuCtxSetCurrent(ctx),"current");
        char bus[32]; check(cuDeviceGetPCIBusId(bus,sizeof(bus),dev),"PCI address");
        unsigned domain,b,d,f;
        if(sscanf(bus,"%x:%x:%x.%x",&domain,&b,&d,&f)!=4) throw std::runtime_error("PCI address parse");
        char info[128]; snprintf(info,sizeof(info),"/proc/driver/nvidia/gpus/%04x:%02x:%02x.%x/information",domain,b,d,f);
        std::ifstream file(info); std::string line; int minor=-1;
        while(std::getline(file,line)) if(line.rfind("Device Minor:",0)==0) minor=std::stoi(line.substr(13));
        if(minor<0) throw std::runtime_error("Cannot match GPU to NVIDIA device node");
        printf("GPU=%d PCI=%s minor=%d nv503c=%d\n",gpu,bus,minor,register_p2p);
        {
            Buffer buf; CUmemAllocationProp prop{}; prop.type=CU_MEM_ALLOCATION_TYPE_PINNED;
            prop.location.type=CU_MEM_LOCATION_TYPE_DEVICE; prop.location.id=dev;
            check(cuMemGetAllocationGranularity(&buf.size,&prop,CU_MEM_ALLOC_GRANULARITY_MINIMUM),"granularity");
            check(cuMemAddressReserve(&buf.va,buf.size,buf.size,0,0),"reserve");
            nv503c_capture_begin(); CUresult rc=cuMemCreate(&buf.mem,buf.size,&prop,0); nv503c_capture_end(); check(rc,"create");
            check(cuMemMap(buf.va,buf.size,0,buf.mem,0),"map"); buf.mapped=true;
            CUmemAccessDesc access{}; access.location=prop.location; access.flags=CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
            check(cuMemSetAccess(buf.va,buf.size,&access,1),"access");
            if(register_p2p) nv503c_register(buf.va,buf.size);
            int cuda_fd=-1;
            rc=cuMemGetHandleForAddressRange(&cuda_fd,buf.va,buf.size,CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD,0);
            printf("CUDA_VMM_EXPORT status=%d fd=%d\n",rc,cuda_fd);
            if(rc==CUDA_SUCCESS) close(cuda_fd);
            std::string device="/dev/nvidia"+std::to_string(minor);
            buf.fd=export_captured_dmabuf(device.c_str(),buf.size);
            int count=0; ibv_device** devices=ibv_get_device_list(&count);
            if(!devices) throw std::runtime_error("No RDMA device list");
            int passed=0,transfers=0;
            for(int i=0;i<count;++i) {
                ibv_context* nic=ibv_open_device(devices[i]); if(!nic) continue;
                ibv_pd* pd=ibv_alloc_pd(nic);
                if(pd) {
                    ibv_mr* mr=ibv_reg_dmabuf_mr(pd,0,buf.size,buf.va,buf.fd,IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE|IBV_ACCESS_REMOTE_READ);
                    int err=errno;
                    printf("NIC=%s DMA_BUF_REGISTRATION=%s errno=%d %s\n",ibv_get_device_name(devices[i]),mr?"PASS":"FAIL",mr?0:err,mr?"":strerror(err));
                    if(mr) {
                        ++passed;
                        try { if(verify_local_rdma(nic,pd,mr,buf.va,buf.size)) ++transfers; }
                        catch(...) { ibv_dereg_mr(mr); ibv_dealloc_pd(pd); ibv_close_device(nic); ibv_free_device_list(devices); throw; }
                        if(ibv_dereg_mr(mr)) throw std::runtime_error("MR cleanup failed");
                    }
                    ibv_dealloc_pd(pd);
                }
                ibv_close_device(nic);
            }
            ibv_free_device_list(devices);
            if(!passed) throw std::runtime_error("No successful DMA-BUF registrations");
            if(!transfers) throw std::runtime_error("Registration passed, but no active NIC completed an RDMA transfer");
            printf("VERIFIED registrations=%d local_rdma_pairs=%d inter_node_tested=false\n",passed,transfers);
        }
        check(cuDevicePrimaryCtxRelease(dev),"release context");
        return 0;
    } catch(const std::exception& e) { fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
