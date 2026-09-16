#pragma once

#include <tynima/rhi/device.h>

#include <memory>

// The backends, one per file; Device::create() picks one.
namespace tynima::rhi {

[[nodiscard]] std::unique_ptr<Device> create_sdl_device(const DeviceDesc& desc);
#if defined(__APPLE__)
[[nodiscard]] std::unique_ptr<Device> create_metal_device(const DeviceDesc& desc);
#endif

} // namespace tynima::rhi
