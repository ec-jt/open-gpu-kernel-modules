// Experimental, process-local CUDA VMM DMA-BUF export for the 615.78.08 ABI.
// Unknown allocations, partial ranges and nonzero export flags fail closed.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cuda.h>
#include <nvos.h>
#include <nv-ioctl.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

using Dlsym = void*(*)(void*,const char*);
static Dlsym raw_dlsym() {
    static auto fn=reinterpret_cast<Dlsym>(dlvsym(RTLD_NEXT,"dlsym","GLIBC_2.2.5"));
    if(!fn) std::abort();
    return fn;
}
template<class T> static T next(const char* name) {
    auto fn=reinterpret_cast<T>(raw_dlsym()(RTLD_NEXT,name));
    if(!fn) { fprintf(stderr,"DMABUF_SHIM missing symbol %s\n",name); std::abort(); }
    return fn;
}
struct Captured { NvHandle client=0,memory=0; unsigned matches=0; };
static thread_local Captured capture;
static thread_local bool capturing=false;
extern "C" int ioctl(int fd,unsigned long request,...) noexcept {
    using Fn=int(*)(int,unsigned long,...);
    static Fn real=next<Fn>("ioctl");
    // This preload is restricted to CUDA/verbs clients using pointer ioctl args.
    va_list args; va_start(args,request); void* arg=va_arg(args,void*); va_end(args);
    int rc=real(fd,request,arg); int saved=errno;
    if(capturing && rc==0 && arg && _IOC_TYPE(request)=='F' && _IOC_NR(request)==0x2b) {
        if(_IOC_SIZE(request)==sizeof(NVOS21_PARAMETERS)) {
            auto* p=static_cast<NVOS21_PARAMETERS*>(arg);
            if(!p->status && p->hClass==0x40) { capture.client=p->hRoot; capture.memory=p->hObjectNew; ++capture.matches; }
        } else if(_IOC_SIZE(request)==sizeof(NVOS64_PARAMETERS)) {
            auto* p=static_cast<NVOS64_PARAMETERS*>(arg);
            if(!p->status && p->hClass==0x40) { capture.client=p->hRoot; capture.memory=p->hObjectNew; ++capture.matches; }
        }
    }
    errno=saved; return rc;
}
struct Allocation { Captured rm; size_t bytes; CUdevice device; };
struct Mapping { CUmemGenericAllocationHandle allocation; size_t bytes; };
struct State { std::recursive_mutex mutex; std::map<CUmemGenericAllocationHandle,Allocation> allocations; std::map<CUdeviceptr,Mapping> mappings; };
static State& state() { static State value; return value; }
static bool supported_version() {
    static bool okay=[](){std::ifstream in("/sys/module/nvidia/version");std::string version;std::getline(in,version);return version=="615.78.08";}();
    return okay;
}
static bool eligible(const CUmemAllocationProp* prop,unsigned long long flags) {
    return supported_version() && prop && !flags && prop->type==CU_MEM_ALLOCATION_TYPE_PINNED &&
           prop->location.type==CU_MEM_LOCATION_TYPE_DEVICE && prop->requestedHandleTypes==CU_MEM_HANDLE_TYPE_NONE;
}
extern "C" CUresult CUDAAPI cuMemCreate(CUmemGenericAllocationHandle* handle,size_t bytes,const CUmemAllocationProp* prop,unsigned long long flags) {
    static auto real=next<decltype(&cuMemCreate)>("cuMemCreate");
    auto& s=state(); std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if(!eligible(prop,flags)) return real(handle,bytes,prop,flags);
    capture={}; capturing=true; CUresult rc=real(handle,bytes,prop,flags); capturing=false;
    if(rc==CUDA_SUCCESS && capture.matches==1) s.allocations[*handle]={capture,bytes,prop->location.id};
    return rc;
}
extern "C" CUresult CUDAAPI cuMemMap(CUdeviceptr ptr,size_t bytes,size_t offset,CUmemGenericAllocationHandle allocation,unsigned long long flags) {
    static auto real=next<decltype(&cuMemMap)>("cuMemMap");
    auto& s=state(); std::lock_guard<std::recursive_mutex> lock(s.mutex);
    CUresult rc=real(ptr,bytes,offset,allocation,flags);
    if(rc==CUDA_SUCCESS) {
        auto it=s.allocations.find(allocation);
        if(!offset && !flags && it!=s.allocations.end() && bytes==it->second.bytes) s.mappings[ptr]={allocation,bytes};
    }
    return rc;
}
extern "C" CUresult CUDAAPI cuMemUnmap(CUdeviceptr ptr,size_t bytes) {
    static auto real=next<decltype(&cuMemUnmap)>("cuMemUnmap");
    auto& s=state(); std::lock_guard<std::recursive_mutex> lock(s.mutex);
    CUresult rc=real(ptr,bytes);
    if(rc==CUDA_SUCCESS) for(auto it=s.mappings.begin();it!=s.mappings.end();) {
        bool overlaps=ptr<it->first+it->second.bytes && it->first<ptr+bytes;
        if(overlaps) it=s.mappings.erase(it); else ++it;
    }
    return rc;
}
extern "C" CUresult CUDAAPI cuMemRelease(CUmemGenericAllocationHandle allocation) {
    static auto real=next<decltype(&cuMemRelease)>("cuMemRelease");
    auto& s=state(); std::lock_guard<std::recursive_mutex> lock(s.mutex);
    CUresult rc=real(allocation);
    // Conservative: early release disables future shim exports even if still mapped.
    if(rc==CUDA_SUCCESS) {
        s.allocations.erase(allocation);
        for(auto it=s.mappings.begin();it!=s.mappings.end();) {
            if(it->second.allocation==allocation) it=s.mappings.erase(it); else ++it;
        }
    }
    return rc;
}
static int gpu_minor(CUdevice device) {
    char pci[32]; if(cuDeviceGetPCIBusId(pci,sizeof(pci),device)!=CUDA_SUCCESS) return -1;
    unsigned domain,bus,slot,function;
    if(sscanf(pci,"%x:%x:%x.%x",&domain,&bus,&slot,&function)!=4) return -1;
    char path[160]; snprintf(path,sizeof(path),"/proc/driver/nvidia/gpus/%04x:%02x:%02x.%x/information",domain,bus,slot,function);
    std::ifstream in(path); std::string line;
    while(std::getline(in,line)) if(line.rfind("Device Minor:",0)==0) { int minor=-1; if(sscanf(line.c_str()+13,"%d",&minor)==1) return minor; }
    return -1;
}
extern "C" CUresult CUDAAPI cuMemGetHandleForAddressRange(void* output,CUdeviceptr ptr,size_t bytes,CUmemRangeHandleType type,unsigned long long flags) {
    static auto real=next<decltype(&cuMemGetHandleForAddressRange)>("cuMemGetHandleForAddressRange");
    CUresult rc=real(output,ptr,bytes,type,flags);
    if(rc!=CUDA_ERROR_NOT_SUPPORTED || !output || !supported_version() || type!=CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD || flags) return rc;
    auto& s=state(); std::lock_guard<std::recursive_mutex> lock(s.mutex);
    auto mapping=s.mappings.find(ptr);
    if(mapping==s.mappings.end() || mapping->second.bytes!=bytes) return rc;
    auto alloc=s.allocations.find(mapping->second.allocation);
    if(alloc==s.allocations.end() || alloc->second.bytes!=bytes) return rc;
    int minor=gpu_minor(alloc->second.device); if(minor<0) return CUDA_ERROR_INVALID_DEVICE;
    std::string path="/dev/nvidia"+std::to_string(minor);
    int device_fd=open(path.c_str(),O_RDWR|O_CLOEXEC); if(device_fd<0) return CUDA_ERROR_OPERATING_SYSTEM;
    nv_ioctl_export_to_dma_buf_fd_t request{};
    request.fd=-1; request.hClient=alloc->second.rm.client; request.totalObjects=request.numObjects=1;
    request.totalSize=bytes; request.mappingType=NV_DMABUF_EXPORT_MAPPING_TYPE_DEFAULT;
    request.handles[0]=alloc->second.rm.memory; request.sizes[0]=bytes;
    int result=ioctl(device_fd,_IOWR('F',NV_ESC_EXPORT_TO_DMABUF_FD,nv_ioctl_export_to_dma_buf_fd_t),&request);
    close(device_fd);
    if(result<0 || request.status || request.fd<0) return CUDA_ERROR_NOT_SUPPORTED;
    if(fcntl(request.fd,F_SETFD,FD_CLOEXEC)<0) {close(request.fd); return CUDA_ERROR_OPERATING_SYSTEM;}
    *static_cast<int*>(output)=request.fd;
    fprintf(stderr,"DMABUF_SHIM exported GPU=%d bytes=%zu fd=%d\n",alloc->second.device,bytes,request.fd);
    return CUDA_SUCCESS;
}
using Proc4=CUresult(*)(const char*,void**,int,cuuint64_t);
using Proc5=CUresult(*)(const char*,void**,int,cuuint64_t,CUdriverProcAddressQueryResult*);
extern "C" CUresult shim_proc4(const char*,void**,int,cuuint64_t) asm("cuGetProcAddress");
extern "C" CUresult shim_proc5(const char*,void**,int,cuuint64_t,CUdriverProcAddressQueryResult*) asm("cuGetProcAddress_v2");
static void* redirect(const char* name,void* original) {
    if(!original || !name) return original;
#define HOOK(symbol) if(!strcmp(name,#symbol)) return reinterpret_cast<void*>(&symbol)
    HOOK(cuMemCreate); HOOK(cuMemMap); HOOK(cuMemUnmap); HOOK(cuMemRelease); HOOK(cuMemGetHandleForAddressRange);
#undef HOOK
    if(!strcmp(name,"cuGetProcAddress")) return reinterpret_cast<void*>(&shim_proc4);
    if(!strcmp(name,"cuGetProcAddress_v2")) return reinterpret_cast<void*>(&shim_proc5);
    return original;
}
extern "C" CUresult shim_proc4(const char* name,void** fn,int version,cuuint64_t flags) {
    static auto real=next<Proc4>("cuGetProcAddress");
    CUresult rc=real(name,fn,version,flags); if(rc==CUDA_SUCCESS && fn) *fn=redirect(name,*fn); return rc;
}
extern "C" CUresult shim_proc5(const char* name,void** fn,int version,cuuint64_t flags,CUdriverProcAddressQueryResult* status) {
    static auto real=next<Proc5>("cuGetProcAddress_v2");
    CUresult rc=real(name,fn,version,flags,status); if(rc==CUDA_SUCCESS && fn) *fn=redirect(name,*fn); return rc;
}
extern "C" void* dlsym(void* handle,const char* name) noexcept { return redirect(name,raw_dlsym()(handle,name)); }
