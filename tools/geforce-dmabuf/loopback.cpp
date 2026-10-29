#include <cuda.h>
#include <infiniband/verbs.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

static void required(bool ok,const char* stage) {
    if(!ok) throw std::runtime_error(std::string(stage)+": "+strerror(errno));
}
static void cuda_ok(CUresult r,const char* stage) {
    if(r) { const char* s=nullptr; cuGetErrorName(r,&s); throw std::runtime_error(std::string(stage)+": "+(s?s:"CUDA error")); }
}
struct LocalPair {
    ibv_cq* cq=nullptr; ibv_qp *a=nullptr,*b=nullptr; ibv_mr* host_mr=nullptr; void* host=nullptr;
    ~LocalPair() {
        if(a) ibv_destroy_qp(a);
        if(b) ibv_destroy_qp(b);
        if(cq) ibv_destroy_cq(cq);
        if(host_mr) ibv_dereg_mr(host_mr);
        free(host);
    }
};
static void ready(ibv_qp* qp,ibv_qp* peer,const ibv_port_attr& port,int gid_index,const ibv_gid& gid) {
    ibv_qp_attr a{}; a.qp_state=IBV_QPS_INIT; a.pkey_index=0; a.port_num=1;
    a.qp_access_flags=IBV_ACCESS_REMOTE_WRITE|IBV_ACCESS_REMOTE_READ;
    required(!ibv_modify_qp(qp,&a,IBV_QP_STATE|IBV_QP_PKEY_INDEX|IBV_QP_PORT|IBV_QP_ACCESS_FLAGS),"QP INIT");
    a={}; a.qp_state=IBV_QPS_RTR; a.path_mtu=port.active_mtu; a.dest_qp_num=peer->qp_num;
    a.rq_psn=0; a.max_dest_rd_atomic=1; a.min_rnr_timer=12;
    a.ah_attr.dlid=port.lid; a.ah_attr.port_num=1;
    if(port.link_layer==IBV_LINK_LAYER_ETHERNET) {
        a.ah_attr.is_global=1; a.ah_attr.grh.dgid=gid;
        a.ah_attr.grh.sgid_index=gid_index; a.ah_attr.grh.hop_limit=1;
    }
    required(!ibv_modify_qp(qp,&a,IBV_QP_STATE|IBV_QP_AV|IBV_QP_PATH_MTU|IBV_QP_DEST_QPN|IBV_QP_RQ_PSN|IBV_QP_MAX_DEST_RD_ATOMIC|IBV_QP_MIN_RNR_TIMER),"QP RTR");
    a={}; a.qp_state=IBV_QPS_RTS; a.timeout=14; a.retry_cnt=3; a.rnr_retry=3; a.sq_psn=0; a.max_rd_atomic=1;
    required(!ibv_modify_qp(qp,&a,IBV_QP_STATE|IBV_QP_TIMEOUT|IBV_QP_RETRY_CNT|IBV_QP_RNR_RETRY|IBV_QP_SQ_PSN|IBV_QP_MAX_QP_RD_ATOMIC),"QP RTS");
}
static void transfer(LocalPair& p,ibv_mr* gpu,CUdeviceptr va,size_t bytes,bool read) {
    ibv_sge sge{}; sge.addr=reinterpret_cast<uint64_t>(p.host); sge.length=bytes; sge.lkey=p.host_mr->lkey;
    ibv_send_wr wr{}; wr.wr_id=read?2:1; wr.sg_list=&sge; wr.num_sge=1;
    wr.opcode=read?IBV_WR_RDMA_READ:IBV_WR_RDMA_WRITE; wr.send_flags=IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr=va; wr.wr.rdma.rkey=gpu->rkey;
    ibv_send_wr* bad=nullptr;
    required(!ibv_post_send(p.a,&wr,&bad),"post RDMA");
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    for(;;) {
        ibv_wc wc{}; int n=ibv_poll_cq(p.cq,1,&wc);
        required(n>=0,"poll CQ");
        if(n) {
            if(wc.status!=IBV_WC_SUCCESS || wc.wr_id!=wr.wr_id)
                throw std::runtime_error(std::string("RDMA completion: ")+ibv_wc_status_str(wc.status)+" vendor="+std::to_string(wc.vendor_err));
            return;
        }
        if(std::chrono::steady_clock::now()>end) throw std::runtime_error("RDMA completion timed out");
        usleep(1000);
    }
}
// Returns false for inactive ports; successful return means hardware DMA and byte checks passed.
bool verify_local_rdma(ibv_context* nic,ibv_pd* pd,ibv_mr* gpu,CUdeviceptr va,size_t bytes) {
    ibv_port_attr port{}; required(!ibv_query_port(nic,1,&port),"query port");
    if(port.state!=IBV_PORT_ACTIVE) { puts("LOCAL_RDMA=SKIP inactive port"); return false; }
    ibv_gid gid{}; int gid_index=-1;
    if(port.link_layer==IBV_LINK_LAYER_ETHERNET) {
        for(int i=0;i<port.gid_tbl_len;++i) {
            if(ibv_query_gid(nic,1,i,&gid)) continue;
            const unsigned char zero[16]{};
            if(memcmp(gid.raw,zero,16)) { gid_index=i; break; }
        }
        required(gid_index>=0,"no usable GID");
    }
    LocalPair p;
    required(!posix_memalign(&p.host,4096,bytes),"host allocation");
    p.host_mr=ibv_reg_mr(pd,p.host,bytes,IBV_ACCESS_LOCAL_WRITE); required(p.host_mr,"host MR");
    p.cq=ibv_create_cq(nic,8,nullptr,nullptr,0); required(p.cq,"CQ");
    ibv_qp_init_attr q{}; q.send_cq=q.recv_cq=p.cq; q.qp_type=IBV_QPT_RC;
    q.cap.max_send_wr=4; q.cap.max_recv_wr=1; q.cap.max_send_sge=q.cap.max_recv_sge=1;
    p.a=ibv_create_qp(pd,&q); required(p.a,"QP A");
    p.b=ibv_create_qp(pd,&q); required(p.b,"QP B");
    ready(p.a,p.b,port,gid_index,gid); ready(p.b,p.a,port,gid_index,gid);
    std::vector<unsigned char> expected(bytes),actual(bytes);
    for(size_t i=0;i<bytes;++i) expected[i]=static_cast<unsigned char>((i*73+(i>>8)+19)&255);
    memcpy(p.host,expected.data(),bytes);
    cuda_ok(cuMemsetD8(va,0xa5,bytes),"GPU poison"); cuda_ok(cuCtxSynchronize(),"sync poison");
    transfer(p,gpu,va,bytes,false);
    cuda_ok(cuMemcpyDtoH(actual.data(),va,bytes),"verify GPU bytes");
    required(actual==expected,"RDMA WRITE byte comparison");
    for(size_t i=0;i<bytes;++i) expected[i]^=0x97;
    cuda_ok(cuMemcpyHtoD(va,expected.data(),bytes),"GPU read-test pattern");
    memset(p.host,0x5a,bytes);
    transfer(p,gpu,va,bytes,true);
    required(!memcmp(p.host,expected.data(),bytes),"RDMA READ byte comparison");
    printf("LOCAL_RDMA=PASS write_bytes=%zu read_bytes=%zu data_verified=true gid_index=%d\n",bytes,bytes,gid_index);
    return true;
}
