CUDA_ROOT ?= /usr/local/cuda
CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
NVIDIA_SOURCE ?= $(abspath ../..)
gds_probe: probe.cpp nv503c.cpp nv503c.hpp experimental_caps.cpp
	$(CXX) $(CXXFLAGS) -I$(CUDA_ROOT)/include -I$(NVIDIA_SOURCE)/src/common/sdk/nvidia/inc probe.cpp nv503c.cpp experimental_caps.cpp -o $@ -rdynamic -L$(CUDA_ROOT)/lib64 -Wl,-rpath,$(CUDA_ROOT)/lib64 -lcufile -lcudart -lcuda -ldl
