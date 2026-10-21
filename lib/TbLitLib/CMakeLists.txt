# The lit preview's renderer on NVRHI's Vulkan backend. It is Qt-free (D28) and built only
# with TB_ENABLE_LIT_PREVIEW.
add_library(TbLitLib STATIC)

target_sources(TbLitLib
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/DeviceChoice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FrameLog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FramePacing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LitSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NvrhiSupport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Swapchain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Swapchain.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SwapchainRules.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VulkanDevice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VulkanDevice.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VulkanInstance.cpp

  PUBLIC FILE_SET headers TYPE HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# nvrhi_vk is public for its public VK_USE_PLATFORM_WIN32_KHR: every file that includes
# vulkan.hpp must see the platform defines NVRHI was compiled with. The Vulkan headers are
# the pinned set (C13, cmake/dependencies/nvrhi.cmake). TbBaseLib brings Result, VmLib and
# fmt; nothing here links Qt.
target_link_libraries(TbLitLib
  PRIVATE
    CompilerConfig
    PrecompileStdHeaders
  PUBLIC
    TbBaseLib
    nvrhi
    nvrhi_vk
    WrapVulkanHeaders::WrapVulkanHeaders
)

# Vulkan is reached only through vulkan.hpp's dynamic dispatcher and the loader is never
# linked (C9), so a call to a static prototype fails to compile. On Windows vulkan.h
# includes windows.h, whose min and max macros NOMINMAX keeps out.
target_compile_definitions(TbLitLib
  PUBLIC
    VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
    VK_NO_PROTOTYPES
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
)

add_subdirectory(test)
