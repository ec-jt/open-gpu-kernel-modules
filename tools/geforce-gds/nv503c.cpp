// Experimental single-buffer/single-thread probe. Uses the installed driver's ABI.
// Registration mechanism: NVIDIA NV503C public headers and mcornea's RDMA experiment.
#include "nv503c.hpp"
#include <nvos.h>
#include <class/cl503c.h>
#include <ctrl/ctrl503c.h>
#include <sys/ioctl.h>
#include <dlfcn.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
using Ioctl = int (*)(int,unsigned long,...);
Ioctl real_ioctl() {
    static Ioctl fn = reinterpret_cast<Ioctl>(dlsym(RTLD_NEXT,"ioctl"));
    if (!fn) { fputs("Cannot resolve ioctl\n",stderr); std::abort(); }
    return fn;
}
struct Alloc { int fd; NvHandle client,parent,object; NvU32 cls,va_flags; };
Alloc objects[4096]; unsigned count;
thread_local bool capture;
Alloc captured[16]; unsigned captured_count;
NvHandle client,subdevice,vaspace,memory,p2p;
int ctl_fd = -1;
bool va_registered,mem_registered;
void track(int fd,NvHandle root,NvHandle parent,NvHandle object,NvU32 cls,NvU32 status,NvP64 params,NvU32 params_size) {
    if (status) return;
    Alloc a{fd,root,parent,object,cls,0};
    if(cls==0x90f1 && params && (params_size==0 || params_size>=8)) {
        auto* va=reinterpret_cast<NV_VASPACE_ALLOCATION_PARAMETERS*>(params);
        a.va_flags=va->flags;
        if(getenv("NV503C_TRACE")) fprintf(stderr,"RM VA object=%x index=%u flags=%x parameter_bytes=%u\n",object,va->index,va->flags,params_size);
    }
    if (count<4096) objects[count++]=a;
    if(capture && captured_count<16) captured[captured_count++]=a;
    if(getenv("NV503C_TRACE") || capture)
        fprintf(stderr,"RM alloc fd=%d client=%x parent=%x object=%x class=%x capture=%d\n",fd,root,parent,object,cls,capture);
}
void checked(int result,NvU32 status,const char* stage) {
    if(result<0 || status) {
        char msg[180]; snprintf(msg,sizeof(msg),"%s: ioctl=%d errno=%d (%s) RM status=0x%x",stage,result,errno,strerror(errno),status);
        throw std::runtime_error(msg);
    }
}
void control(NvU32 command,void* params,NvU32 bytes,const char* stage) {
    NVOS54_PARAMETERS c{}; c.hClient=client; c.hObject=p2p; c.cmd=command;
    c.params=reinterpret_cast<NvP64>(params); c.paramsSize=bytes;
    int r=real_ioctl()(ctl_fd,_IOWR('F',0x2a,NVOS54_PARAMETERS),&c);
    checked(r,c.status,stage);
}
}
extern "C" int ioctl(int fd,unsigned long request,...) noexcept {
    va_list args; va_start(args,request); void* arg=va_arg(args,void*); va_end(args);
    int ret=real_ioctl()(fd,request,arg); int saved=errno;
    if(ret==0 && arg && _IOC_TYPE(request)=='F' && _IOC_NR(request)==0x2b) {
        if(_IOC_SIZE(request)==sizeof(NVOS21_PARAMETERS)) {
            auto* a=static_cast<NVOS21_PARAMETERS*>(arg);
            track(fd,a->hRoot,a->hObjectParent,a->hObjectNew,a->hClass,a->status,a->pAllocParms,a->paramsSize);
        } else if(_IOC_SIZE(request)==sizeof(NVOS64_PARAMETERS)) {
            auto* a=static_cast<NVOS64_PARAMETERS*>(arg);
            track(fd,a->hRoot,a->hObjectParent,a->hObjectNew,a->hClass,a->status,a->pAllocParms,a->paramsSize);
        } else if(getenv("NV503C_TRACE")) fprintf(stderr,"RM allocation unhandled ioctl size=%u\n",static_cast<unsigned>(_IOC_SIZE(request)));
    }
    errno=saved; return ret;
}
void nv503c_capture_begin() { captured_count=0; capture=true; }
void nv503c_capture_end() { capture=false; }
void nv503c_register(std::uintptr_t address,std::size_t bytes) {
    if(p2p) throw std::runtime_error("NV503C already registered");
    // cuMemCreate must expose one physical-memory allocation. Refuse ambiguity.
    Alloc mem{}; unsigned matches=0;
    for(unsigned i=0;i<captured_count;++i) {
        if(captured[i].cls==0x40) { mem=captured[i]; ++matches; }
    }
    if(matches!=1) throw std::runtime_error("Expected one captured RM memory allocation; found "+std::to_string(matches));
    NvHandle dev=mem.parent;
    client=mem.client; memory=mem.object; ctl_fd=mem.fd; subdevice=vaspace=0;
    unsigned vas=0;
    for(unsigned i=0;i<count;++i) {
        const auto& a=objects[i];
        if(a.client!=client || a.parent!=dev) continue;
        if(a.cls==0x2080) subdevice=a.object;
        // CUDA/UVM owns the application VA space; the driver's separate
        // internal space has neither EXTERNALLY_OWNED nor PAGE_FAULTING set.
        if(a.cls==0x90f1 && (a.va_flags & NV_VASPACE_ALLOCATION_FLAGS_IS_EXTERNALLY_OWNED)
                        && (a.va_flags & NV_VASPACE_ALLOCATION_FLAGS_ENABLE_PAGE_FAULTING)) { vaspace=a.object; ++vas; }
    }
    if(!subdevice || vas!=1) throw std::runtime_error("Missing or ambiguous CUDA RM subdevice/VA-space hierarchy");
    fprintf(stderr,"NV503C selected client=%x device=%x subdevice=%x vaspace=%x memory=%x\n",client,dev,subdevice,vaspace,memory);
    NV503C_ALLOC_PARAMETERS params{}; params.flags=NV503C_ALLOC_PARAMETERS_FLAGS_TYPE_BAR1;
    NVOS21_PARAMETERS a{}; a.hRoot=client; a.hObjectParent=subdevice;
    a.hObjectNew=0; a.hClass=NV50_THIRD_PARTY_P2P;
    a.pAllocParms=reinterpret_cast<NvP64>(&params); a.paramsSize=sizeof(params);
    int result=real_ioctl()(ctl_fd,_IOWR('F',0x2b,NVOS21_PARAMETERS),&a);
    checked(result,a.status,"NV503C alloc"); p2p=a.hObjectNew;
    NV503C_CTRL_REGISTER_VA_SPACE_PARAMS va{}; va.hVASpace=vaspace;
    control(NV503C_CTRL_CMD_REGISTER_VA_SPACE,&va,sizeof(va),"NV503C register VA"); va_registered=true;
    NV503C_CTRL_REGISTER_VIDMEM_PARAMS vm{}; vm.hMemory=memory; vm.address=address; vm.size=bytes;
    control(NV503C_CTRL_CMD_REGISTER_VIDMEM,&vm,sizeof(vm),"NV503C register memory"); mem_registered=true;
    fprintf(stderr,"NV503C registered object=%x bytes=%zu (not yet proof of direct DMA)\n",p2p,bytes);
}
void nv503c_unregister() noexcept {
    if(!p2p) return;
    try {
        if(mem_registered) {
            NV503C_CTRL_UNREGISTER_VIDMEM_PARAMS vm{}; vm.hMemory=memory;
            control(NV503C_CTRL_CMD_UNREGISTER_VIDMEM,&vm,sizeof(vm),"NV503C unregister memory"); mem_registered=false;
        }
        if(va_registered) {
            NV503C_CTRL_UNREGISTER_VA_SPACE_PARAMS va{}; va.hVASpace=vaspace;
            control(NV503C_CTRL_CMD_UNREGISTER_VA_SPACE,&va,sizeof(va),"NV503C unregister VA"); va_registered=false;
        }
        NVOS00_PARAMETERS f{}; f.hRoot=client; f.hObjectParent=subdevice; f.hObjectOld=p2p;
        int r=real_ioctl()(ctl_fd,_IOWR('F',0x29,NVOS00_PARAMETERS),&f);
        checked(r,f.status,"NV503C free"); p2p=0;
    } catch(const std::exception& e) { fprintf(stderr,"NV503C cleanup: %s\n",e.what()); }
}
namespace { int selected_device = -1; }
bool nv503c_ready() noexcept { return p2p && va_registered && mem_registered; }
void nv503c_set_device(int device) noexcept { selected_device=device; }
int nv503c_device() noexcept { return selected_device; }
