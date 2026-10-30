CUDA_ROOT ?= /usr/local/cuda
NVIDIA_SOURCE ?= $(abspath ../../..)
CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
CPPFLAGS += -I$(CUDA_ROOT)/include -I$(NVIDIA_SOURCE)/src/common/sdk/nvidia/inc -I$(NVIDIA_SOURCE)/src/nvidia/arch/nvalloc/unix/include
LDLIBS += -L$(CUDA_ROOT)/lib64 -Wl,-rpath,$(CUDA_ROOT)/lib64 -lcuda -ldl -pthread
all: libgeforce-dmabuf-shim.so api-probe
libgeforce-dmabuf-shim.so: export-shim.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -fPIC -shared $< -o $@ $(LDLIBS)
api-probe: api-probe.cpp ../loopback.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ -o $@ $(LDLIBS) -libverbs
clean:
	rm -f libgeforce-dmabuf-shim.so api-probe
.PHONY: all clean
