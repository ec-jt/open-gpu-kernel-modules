CUDA_ROOT ?= /usr/local/cuda
NVIDIA_SOURCE ?= $(abspath ../..)
CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
GDS_SOURCE := ../geforce-gds

dmabuf_probe: probe.cpp loopback.cpp export.cpp $(GDS_SOURCE)/nv503c.cpp $(GDS_SOURCE)/nv503c.hpp
	$(CXX) $(CXXFLAGS) -I$(GDS_SOURCE) -I$(CUDA_ROOT)/include -I$(NVIDIA_SOURCE)/src/common/sdk/nvidia/inc -I$(NVIDIA_SOURCE)/src/nvidia/arch/nvalloc/unix/include probe.cpp loopback.cpp export.cpp $(GDS_SOURCE)/nv503c.cpp -o $@ -rdynamic -L$(CUDA_ROOT)/lib64 -Wl,-rpath,$(CUDA_ROOT)/lib64 -lcuda -libverbs -ldl
