# Vulkan-Headers

- Upstream: https://github.com/KhronosGroup/Vulkan-Headers
- Version: 1.3.204 (`VK_HEADER_VERSION 204` and
  `VK_HEADER_VERSION_COMPLETE` 1.3, `include/vulkan/vulkan_core.h:75-78`).
- Files: `include/vulkan/` and `include/vk_video/`.
- Licence: Apache-2.0 (the SPDX line in each C header; the Apache notice in
  `vk_icd.h`, `vk_layer.h` and `vk_sdk_platform.h`); the C++ headers
  `vulkan*.hpp` are `Apache-2.0 OR MIT`. Copy shipped as
  `docs/licences/vulkan-headers.txt`.
- Use: compile-time headers for `openpete_rhi` and `openpete_imgui`
  (`core/rhi/CMakeLists.txt:13`, `core/CMakeLists.txt:522`); no Vulkan
  library is linked, volk loads it at runtime.
