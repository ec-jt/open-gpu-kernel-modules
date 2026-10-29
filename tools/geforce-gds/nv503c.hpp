#pragma once
#include <cstddef>
#include <cstdint>
void nv503c_capture_begin();
void nv503c_capture_end();
void nv503c_register(std::uintptr_t address, std::size_t bytes);
void nv503c_unregister() noexcept;
bool nv503c_ready() noexcept;
void nv503c_set_device(int device) noexcept;
int nv503c_device() noexcept;
struct CapturedGpuMemory { std::uint32_t client, memory; };
CapturedGpuMemory nv503c_captured_memory();
