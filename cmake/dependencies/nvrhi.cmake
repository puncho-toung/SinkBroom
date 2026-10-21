# NVRHI with only its Vulkan backend, for the lit preview (TB_ENABLE_LIT_PREVIEW). It is
# pinned to the commit of the engine's neo/extern/nvrhi gitlink, so the preview renders
# through the same layer as the game. Of NVRHI's submodules only the Vulkan headers are
# fetched.
#
# NVRHI declares cmake_minimum_required 3.10, which leaves CMP0077 unset, and its option()
# calls would then drop the normal variables CPM sets for OPTIONS. The policy default makes
# them honour those variables. NVRHI declares its libraries STATIC or SHARED itself
# (NVRHI_BUILD_SHARED), so the global BUILD_SHARED_LIBS does not reach them.
#
# On Windows the checkout's paths are long: a build folder deep in the disk can pass
# MAX_PATH while NVRHI and its Vulkan headers are fetched. Configure such a folder with
# -DFETCHCONTENT_BASE_DIR=<short path>, as the SinkBroom configure line does.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
CPMAddPackage(
  URI "gh:RobertBeckebans/nvrhi#3d81b755010ea27727a39eb53f8e945c358bb058"
  GIT_SUBMODULES "thirdparty/Vulkan-Headers"
  PATCHES "patches/nvrhi-strip-msvc-flags.patch"
  OPTIONS
    "NVRHI_BUILD_SHARED OFF"
    "NVRHI_INSTALL OFF"
    "NVRHI_WITH_VULKAN ON"
    "NVRHI_WITH_DX11 OFF"
    "NVRHI_WITH_DX12 OFF"
    "NVRHI_WITH_RTXMU OFF"
    # the session wraps its device in NVRHI's validation layer in Debug
    "NVRHI_WITH_VALIDATION ON"
    "NVRHI_WITH_AFTERMATH OFF"
    "NVRHI_WITH_NVAPI OFF"
)
unset(CMAKE_POLICY_DEFAULT_CMP0077)

# nvrhi_vk calls NVRHI's common code without linking it; the edge lets single-pass linkers
# resolve that whatever order the two are listed in.
target_link_libraries(nvrhi_vk PUBLIC nvrhi)

suppress_dependency_warnings(nvrhi)
apply_sanitizer_options(nvrhi)
suppress_dependency_warnings(nvrhi_vk)
apply_sanitizer_options(nvrhi_vk)

# One Vulkan header set (C13). NVRHI adds its bundled Vulkan-Headers unless a target named
# Vulkan-Headers or Vulkan::Headers already exists, and nvrhi_vk compiles against it. Qt's
# FindWrapVulkanHeaders returns early when WrapVulkanHeaders::WrapVulkanHeaders exists, and
# Qt6::Gui links that target, so defining it on the same headers before find_package(Qt6)
# makes Qt and NVRHI use one set, even on a machine without a Vulkan SDK. Nothing links the
# Vulkan loader (C9): Qt loads it at runtime.
set(_tb_vulkan_headers_dir "${nvrhi_SOURCE_DIR}/thirdparty/Vulkan-Headers")
if(TARGET Vulkan-Headers)
  get_target_property(_tb_nvrhi_vulkan_headers_dir Vulkan-Headers SOURCE_DIR)
endif()
if(NOT "${_tb_nvrhi_vulkan_headers_dir}" STREQUAL "${_tb_vulkan_headers_dir}")
  message(FATAL_ERROR "NVRHI does not use its bundled Vulkan headers")
endif()

add_library(WrapVulkanHeaders::WrapVulkanHeaders INTERFACE IMPORTED)
target_include_directories(WrapVulkanHeaders::WrapVulkanHeaders
  INTERFACE "${_tb_vulkan_headers_dir}/include"
)
unset(_tb_nvrhi_vulkan_headers_dir)
unset(_tb_vulkan_headers_dir)
