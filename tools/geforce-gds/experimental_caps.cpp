// Opt-in, process-local test override. Never interpret this reported capability
// as hardware evidence: success requires real kernel mapping and I/O counters.
#include "nv503c.hpp"
#include <cuda.h>
#include <dlfcn.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>
using Dlsym = void*(*)(void*,const char*);
static Dlsym actual_dlsym() {
    static auto fn=reinterpret_cast<Dlsym>(dlvsym(RTLD_NEXT,"dlsym","GLIBC_2.2.5"));
    if(!fn) std::abort();
    return fn;
}
using Attribute=CUresult(*)(int*,CUdevice_attribute,CUdevice);
static Attribute real_attribute;
static CUresult attribute(int* result,CUdevice_attribute attr,CUdevice dev) {
    CUresult r=real_attribute(result,attr,dev);
    if(r==CUDA_SUCCESS && attr==CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_SUPPORTED && dev==nv503c_device()
       && nv503c_ready() && getenv("GDS_EXPERIMENTAL_CAPABILITY_OVERRIDE")) {
        fprintf(stderr,"EXPERIMENTAL: GPU %d RDMA capability gate %d -> 1 after NV503C registration; direct DMA still unproven\n",dev,*result);
        *result=1;
    }
    return r;
}
static void* replace(const char* name,void* p) {
    if(p && !strcmp(name,"cuDeviceGetAttribute")) {
        real_attribute=reinterpret_cast<Attribute>(p); return reinterpret_cast<void*>(&attribute);
    }
    return p;
}
using Proc4=CUresult(*)(const char*,void**,int,cuuint64_t);
using Proc5=CUresult(*)(const char*,void**,int,cuuint64_t,CUdriverProcAddressQueryResult*);
static Proc4 real_proc4; static Proc5 real_proc5;
using MountString=const char*(*)(void*);
static MountString real_options,real_source,real_fstype;
static const char* mount_options(void* fs) {
    const char* options=real_options(fs);
    if(!options || strstr(options,"data=")) return options;
    const char* type=real_fstype(fs); const char* source=real_source(fs);
    if(!type || strcmp(type,"ext4") || !source || strncmp(source,"/dev/",5)) return options;
    std::string dev(source+5);
    if(dev.empty() || dev.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos) return options;
    // Supply a default omitted from the mount table ONLY when the live ext4
    // options file confirms that exact mode. Do not skip filesystem checks.
    std::ifstream active("/proc/fs/ext4/"+dev+"/options"); std::string line; bool ordered=false;
    while(std::getline(active,line)) {
        if(line=="data=ordered") ordered=true;
        else if(line.rfind("data=",0)==0) return options;
    }
    if(!ordered) return options;
    thread_local std::string normalized;
    normalized=options; normalized+=",data=ordered";
    fprintf(stderr,"EXT4: %s data=ordered verified in /proc/fs/ext4; supplied omitted default to cuFile mount parser\n",source);
    return normalized.c_str();
}
static CUresult proc4(const char* name,void** fn,int version,cuuint64_t flags) {
    CUresult r=real_proc4(name,fn,version,flags); if(r==CUDA_SUCCESS && fn) *fn=replace(name,*fn); return r;
}
static CUresult proc5(const char* name,void** fn,int version,cuuint64_t flags,CUdriverProcAddressQueryResult* status) {
    CUresult r=real_proc5(name,fn,version,flags,status); if(r==CUDA_SUCCESS && fn) *fn=replace(name,*fn); return r;
}
extern "C" void* dlsym(void* handle,const char* name) noexcept {
    void* p=actual_dlsym()(handle,name);
    if(!p) return p;
    if(!strcmp(name,"mnt_fs_get_options") && getenv("GDS_NORMALIZE_EXT4_OPTIONS")) {
        real_options=reinterpret_cast<MountString>(p);
        real_source=reinterpret_cast<MountString>(actual_dlsym()(handle,"mnt_fs_get_source"));
        real_fstype=reinterpret_cast<MountString>(actual_dlsym()(handle,"mnt_fs_get_fstype"));
        if(real_source && real_fstype) return reinterpret_cast<void*>(&mount_options);
    }
    if(!getenv("GDS_EXPERIMENTAL_CAPABILITY_OVERRIDE")) return p;
    if(!strcmp(name,"cuGetProcAddress")) { real_proc4=reinterpret_cast<Proc4>(p); return reinterpret_cast<void*>(&proc4); }
    if(!strcmp(name,"cuGetProcAddress_v2")) { real_proc5=reinterpret_cast<Proc5>(p); return reinterpret_cast<void*>(&proc5); }
    return replace(name,p);
}
