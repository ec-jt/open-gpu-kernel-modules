// Client uses only public CUDA export APIs; no RM capture or direct ioctl calls.
#include <cuda.h>
#include <infiniband/verbs.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <stdexcept>
bool verify_local_rdma(ibv_context*,ibv_pd*,ibv_mr*,CUdeviceptr,std::size_t);
static void check(CUresult rc,const char* what){if(rc){const char* n=nullptr;cuGetErrorName(rc,&n);throw std::runtime_error(std::string(what)+": "+(n?n:"error"));}}
struct Api {
 decltype(&cuMemCreate) create=&cuMemCreate;
 decltype(&cuMemMap) map=&cuMemMap;
 decltype(&cuMemUnmap) unmap=&cuMemUnmap;
 decltype(&cuMemRelease) release=&cuMemRelease;
 decltype(&cuMemGetHandleForAddressRange) export_fd=&cuMemGetHandleForAddressRange;
};
static void* resolve(void* library,const char* name,int mode){
 if(mode==1) {void* p=dlsym(library,name); if(!p)throw std::runtime_error(name);return p;}
 void* p=nullptr;
 if(mode==2){CUdriverProcAddressQueryResult status;check(cuGetProcAddress(name,&p,13000,0,&status),"getproc_v2");if(status!=CU_GET_PROC_ADDRESS_SUCCESS)throw std::runtime_error("getproc status");}
 else {using Old=CUresult(*)(const char*,void**,int,cuuint64_t);auto old=reinterpret_cast<Old>(dlsym(library,"cuGetProcAddress"));if(!old)throw std::runtime_error("old getproc");check(old(name,&p,13000,0),"getproc_v1");}
 if(!p)throw std::runtime_error("null CUDA API pointer");
 return p;
}
struct Buffer {
 Api* api; CUdeviceptr ptr=0; CUmemGenericAllocationHandle handle=0;size_t bytes=0;bool mapped=false;
 explicit Buffer(Api* a):api(a){}
 ~Buffer(){if(mapped)api->unmap(ptr,bytes);if(handle)api->release(handle);if(ptr)cuMemAddressFree(ptr,bytes);}
 void allocate(CUdevice gpu,size_t multiplier){CUmemAllocationProp p{};p.type=CU_MEM_ALLOCATION_TYPE_PINNED;p.location.type=CU_MEM_LOCATION_TYPE_DEVICE;p.location.id=gpu;
 size_t granularity;check(cuMemGetAllocationGranularity(&granularity,&p,CU_MEM_ALLOC_GRANULARITY_MINIMUM),"granularity");bytes=granularity*multiplier;
 check(cuMemAddressReserve(&ptr,bytes,granularity,0,0),"reserve");check(api->create(&handle,bytes,&p,0),"create");check(api->map(ptr,bytes,0,handle,0),"map");mapped=true;
 CUmemAccessDesc access{};access.location=p.location;access.flags=CU_MEM_ACCESS_FLAGS_PROT_READWRITE;check(cuMemSetAccess(ptr,bytes,&access,1),"access");}
};
static void rejected(CUresult rc,int fd,const char* name){if(rc==CUDA_SUCCESS){if(fd>=0)close(fd);throw std::runtime_error(std::string("unexpected successful export: ")+name);}printf("REJECTED %s status=%d\n",name,rc);}
static int verify_fd(Buffer& b){int fd=-1;check(b.api->export_fd(&fd,b.ptr,b.bytes,CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD,0),"ordinary CUDA export");if(fd<0)throw std::runtime_error("bad fd");
 int count=0,pairs=0;ibv_device** devices=ibv_get_device_list(&count);if(!devices){close(fd);throw std::runtime_error("verbs devices");}
 try {for(int i=0;i<count;i++){ibv_context* nic=ibv_open_device(devices[i]);if(!nic)continue;ibv_pd* pd=ibv_alloc_pd(nic);if(!pd){ibv_close_device(nic);continue;}
 ibv_mr* mr=ibv_reg_dmabuf_mr(pd,0,b.bytes,b.ptr,fd,IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE|IBV_ACCESS_REMOTE_READ);
 if(!mr){ibv_dealloc_pd(pd);ibv_close_device(nic);throw std::runtime_error("DMA-BUF registration");}
 try{if(verify_local_rdma(nic,pd,mr,b.ptr,b.bytes))pairs++;}catch(...){ibv_dereg_mr(mr);ibv_dealloc_pd(pd);ibv_close_device(nic);throw;}
 if(ibv_dereg_mr(mr)){ibv_dealloc_pd(pd);ibv_close_device(nic);throw std::runtime_error("MR cleanup");}ibv_dealloc_pd(pd);ibv_close_device(nic);
 }}catch(...){ibv_free_device_list(devices);close(fd);throw;}
 ibv_free_device_list(devices);close(fd);if(!pairs)throw std::runtime_error("no active NIC transfer");return pairs;
}
int main(int argc,char** argv){try{
 int index=argc>1?std::stoi(argv[1]):0;check(cuInit(0),"init");CUdevice gpu;check(cuDeviceGet(&gpu,index),"device");CUcontext context;check(cuDevicePrimaryCtxRetain(&context,gpu),"retain");check(cuCtxSetCurrent(context),"context");
 void* library=dlopen("libcuda.so.1",RTLD_NOW|RTLD_LOCAL);if(!library)throw std::runtime_error("dlopen");int total=0;
 for(int mode=0;mode<4;mode++){
 Api api;if(mode){api.create=reinterpret_cast<decltype(api.create)>(resolve(library,"cuMemCreate",mode));api.map=reinterpret_cast<decltype(api.map)>(resolve(library,"cuMemMap",mode));api.unmap=reinterpret_cast<decltype(api.unmap)>(resolve(library,"cuMemUnmap",mode));api.release=reinterpret_cast<decltype(api.release)>(resolve(library,"cuMemRelease",mode));api.export_fd=reinterpret_cast<decltype(api.export_fd)>(resolve(library,"cuMemGetHandleForAddressRange",mode));}
 Buffer first(&api),second(&api);first.allocate(gpu,1);second.allocate(gpu,2);
 total+=verify_fd(first);total+=verify_fd(second);
 int fd=-1;CUresult rc=api.export_fd(&fd,first.ptr+65536,first.bytes-65536,CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD,0);rejected(rc,fd,"partial range");
 fd=-1;rc=api.export_fd(&fd,first.ptr,first.bytes,CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD,1);rejected(rc,fd,"nonzero flags");
 check(api.unmap(first.ptr,first.bytes),"unmap");first.mapped=false;fd=-1;rc=api.export_fd(&fd,first.ptr,first.bytes,CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD,0);rejected(rc,fd,"unmapped address");
 check(api.release(second.handle),"early release");second.handle=0;fd=-1;rc=api.export_fd(&fd,second.ptr,second.bytes,CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD,0);rejected(rc,fd,"released handle");
 printf("MODE_PASS gpu=%d mode=%d\n",index,mode);
 }
 CUdeviceptr legacy;check(cuMemAlloc(&legacy,65536),"legacy allocation");int fd=-1;CUresult rc=cuMemGetHandleForAddressRange(&fd,legacy,65536,CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD,0);cuMemFree(legacy);rejected(rc,fd,"untracked cuMemAlloc");
 dlclose(library);check(cuDevicePrimaryCtxRelease(gpu),"release context");printf("VERIFIED ordinary_export_modes=4 local_rdma_pairs=%d negative_checks=17 gpu=%d\n",total,index);return 0;
}catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
