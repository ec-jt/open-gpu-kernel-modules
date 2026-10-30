#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <cufile.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <fstream>
#include "nv503c.hpp"

// Small, read-only, synchronous correctness probe. A successful cuFile call
// is deliberately NOT reported as proof of host-memory bypass.
static void runtime(cudaError_t e) {
    if (e != cudaSuccess) throw std::runtime_error(cudaGetErrorString(e));
}
static void driver(CUresult e, const char *stage = "CUDA driver") {
    if (e != CUDA_SUCCESS) {
        const char *s = nullptr;
        cuGetErrorString(e, &s);
        throw std::runtime_error(std::string(stage) + ": " + (s ? s : "CUDA driver error"));
    }
}
static void filecheck(CUfileError_t e, const char *stage) {
    if (e.err != CU_FILE_SUCCESS)
        throw std::runtime_error(std::string(stage) + ": cuFile=" +
            std::to_string(e.err) + " cuda=" + std::to_string(e.cu_err));
}
static size_t number(const char *s, size_t minimum, size_t maximum) {
    std::string input(s);
    if (input.empty() || input.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Expected an unsigned decimal integer");
    size_t n = std::stoull(input);
    if (n < minimum || n > maximum) throw std::runtime_error("Argument out of range");
    return n;
}
static void read_exact(int fd, void *p, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = pread(fd, static_cast<char *>(p) + done, n - done, done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) throw std::runtime_error(r == 0 ? "Unexpected EOF" : strerror(errno));
        done += static_cast<size_t>(r);
    }
}
struct Resources {
    int fd = -1;
    void *gpu = nullptr, *host = nullptr;
    CUdeviceptr address = 0;
    CUmemGenericAllocationHandle allocation = 0;
    size_t allocated = 0;
    CUfileHandle_t file{};
    bool vmm = false, mapped = false, host_registered = false;
    bool opened = false, file_registered = false, gpu_registered = false;
    ~Resources() {
        if (gpu_registered) cuFileBufDeregister(gpu);
        if (file_registered) cuFileHandleDeregister(file);
        if (opened) cuFileDriverClose();
        nv503c_unregister();
        if (host_registered) cudaHostUnregister(host);
        free(host);
        if (vmm) {
            if (mapped) cuMemUnmap(address, allocated);
            if (allocation) cuMemRelease(allocation);
            if (address) cuMemAddressFree(address, allocated);
        } else if (gpu) cudaFree(gpu);
        if (fd >= 0) close(fd);
    }
};
int main(int argc, char **argv) {
    if (argc < 3 || argc > 7) {
        std::cerr << "Usage: gds_probe FILE gds|staged|register [GPU=0] [MiB=16] [ITERATIONS=3] [malloc|vmm|nv503c]\n";
        return 2;
    }
    try {
        const std::string mode(argv[2]);
        const std::string allocator = argc > 6 ? argv[6] : "malloc";
        if (mode != "gds" && mode != "staged" && mode != "register") throw std::runtime_error("Invalid mode");
        if (allocator != "malloc" && allocator != "vmm" && allocator != "nv503c") throw std::runtime_error("Invalid allocator");
        const int device = argc > 3 ? number(argv[3], 0, 255) : 0;
        nv503c_set_device(device);
        const size_t bytes = (argc > 4 ? number(argv[4], 1, 1024) : 16) * 1024 * 1024;
        const size_t iterations = argc > 5 ? number(argv[5], 1, 1000) : 3;
        // Override inherited fallback settings. Also use the supplied strict
        // per-process cuFile JSON when launching; never edit /etc/cufile.json.
        if (mode != "staged") {
            setenv("CUFILE_ALLOW_COMPAT_MODE", "false", 1);
            setenv("CUFILE_FORCE_COMPAT_MODE", "false", 1);
        }
        Resources r;
        r.fd = open(argv[1], O_RDONLY | O_DIRECT | O_NOFOLLOW);
        if (r.fd < 0) throw std::runtime_error(strerror(errno));
        struct stat st{};
        if (fstat(r.fd, &st) || !S_ISREG(st.st_mode) || st.st_size < static_cast<off_t>(bytes))
            throw std::runtime_error("Need an existing regular file at least as large as the transfer");
        // cuFile rejects O_NOFOLLOW in F_GETFL. Reopen the already validated
        // descriptor, not the pathname, to retain protection against symlink races.
        if (mode == "gds") {
            const std::string procfd = "/proc/self/fd/" + std::to_string(r.fd);
            int directfd = open(procfd.c_str(), O_RDONLY | O_DIRECT);
            if (directfd < 0) throw std::runtime_error(strerror(errno));
            struct stat reopened{};
            if (fstat(directfd, &reopened) || reopened.st_dev != st.st_dev || reopened.st_ino != st.st_ino) {
                close(directfd);
                throw std::runtime_error("Reopened descriptor identity mismatch");
            }
            close(r.fd);
            r.fd = directfd;
        }
        if (posix_memalign(&r.host, 4096, bytes)) throw std::runtime_error("Host allocation failed");
        read_exact(r.fd, r.host, bytes);
        std::vector<unsigned char> expected(bytes), actual(bytes);
        memcpy(expected.data(), r.host, bytes);
        runtime(cudaSetDevice(device));
        runtime(cudaFree(nullptr)); // establish the runtime primary context
        runtime(cudaHostRegister(r.host, bytes, cudaHostRegisterDefault));
        r.host_registered = true;
        if (allocator != "malloc") {
            r.vmm = true;
            CUmemAllocationProp prop{};
            prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
            prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
            prop.location.id = device;
            size_t granularity = 0;
            driver(cuMemGetAllocationGranularity(&granularity, &prop, CU_MEM_ALLOC_GRANULARITY_MINIMUM));
            r.allocated = ((bytes + granularity - 1) / granularity) * granularity;
            driver(cuMemAddressReserve(&r.address, r.allocated, granularity, 0, 0));
            if (allocator == "nv503c") nv503c_capture_begin();
            CUresult allocation_result = cuMemCreate(&r.allocation, r.allocated, &prop, 0);
            if (allocator == "nv503c") nv503c_capture_end();
            driver(allocation_result, "cuMemCreate");
            driver(cuMemMap(r.address, r.allocated, 0, r.allocation, 0));
            r.mapped = true;
            CUmemAccessDesc access{};
            access.location = prop.location;
            access.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
            driver(cuMemSetAccess(r.address, r.allocated, &access, 1));
            r.gpu = reinterpret_cast<void *>(r.address);
        } else runtime(cudaMalloc(&r.gpu, bytes));
        if (allocator == "nv503c") nv503c_register(reinterpret_cast<std::uintptr_t>(r.gpu), r.allocated);
        unsigned int sync = 1;
        CUresult sync_result = cuPointerSetAttribute(&sync, CU_POINTER_ATTRIBUTE_SYNC_MEMOPS,
                                    reinterpret_cast<CUdeviceptr>(r.gpu));
        if (r.vmm && sync_result == CUDA_ERROR_NOT_SUPPORTED)
            std::cerr << "VMM SYNC_MEMOPS unavailable; this probe uses explicit synchronization and synchronous I/O only.\n";
        else driver(sync_result, "SYNC_MEMOPS");
        if (mode != "staged") {
            filecheck(cuFileDriverOpen(), "driver-open"); r.opened = true;
            filecheck(cuFileBufRegister(r.gpu, bytes, 0), "buffer-register"); r.gpu_registered = true;
            if (mode == "register") {
                std::ifstream stats("/proc/driver/nvidia-fs/stats");
                if (stats) std::cout << stats.rdbuf();
                std::ifstream modules("/proc/driver/nvidia-fs/modules");
                if (modules) std::cout << "Registered storage hooks:\n" << modules.rdbuf();
                std::cout << "PASS: buffer registration only; no storage transfer performed.\n";
                return 0;
            }
            CUfileDescr_t desc{};
            desc.type = CU_FILE_HANDLE_TYPE_OPAQUE_FD;
            desc.handle.fd = r.fd;
            filecheck(cuFileHandleRegister(&r.file, &desc), "file-register"); r.file_registered = true;
        }
        for (size_t i = 0; i < iterations; ++i) {
            runtime(cudaMemset(r.gpu, 0xA5, bytes));
            runtime(cudaDeviceSynchronize());
            const auto start = std::chrono::steady_clock::now();
            if (mode == "gds") {
                ssize_t got = cuFileRead(r.file, r.gpu, bytes, 0, 0);
                if (got != static_cast<ssize_t>(bytes))
                    throw std::runtime_error("cuFileRead returned " + std::to_string(got) +
                                             "; errno=" + std::to_string(errno));
            } else {
                read_exact(r.fd, r.host, bytes);
                runtime(cudaMemcpy(r.gpu, r.host, bytes, cudaMemcpyHostToDevice));
            }
            runtime(cudaDeviceSynchronize());
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            runtime(cudaMemcpy(actual.data(), r.gpu, bytes, cudaMemcpyDeviceToHost));
            if (actual != expected) throw std::runtime_error("GPU destination data mismatch");
            std::cout << "mode=" << mode << " allocator=" << allocator << " iteration=" << i
                      << " bytes=" << bytes << " seconds=" << seconds
                      << " GB_per_s=" << bytes / seconds / 1e9 << " data_verified=true\n";
        }
        std::cout << "PASS: data integrity only; inspect per-run GDS counters/logs to establish transfer path.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
