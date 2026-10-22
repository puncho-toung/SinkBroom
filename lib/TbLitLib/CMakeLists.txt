# The lit preview's renderer on NVRHI's Vulkan backend. It is Qt-free (D28) and built only
# with TB_ENABLE_LIT_PREVIEW.

# the engine's material parser, copied verbatim, behind a std-only facade (D12, D31)
add_subdirectory(idtech)

add_library(TbLitLib STATIC)

target_sources(TbLitLib
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AreaTree.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/BprocModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ChunkBuilder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CpuPool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/DeviceChoice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/EntityState.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FrameLog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FramePacer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FrameTimers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FrameTimers.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GameFileSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GameFile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GpuDevice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GpuDevice.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LightData.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LightParse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LightRecord.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LitSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LiveSession.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NumberText.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NvrhiSupport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ProcFile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Projection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderThread.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderThread.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ResourceContainer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneGeometry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SessionEvents.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SessionEvents.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/StackThread.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/StackThread.h
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
# fmt; TbFsLib reads the game's .pk4 files (D55); nothing here links Qt.
target_link_libraries(TbLitLib
  PRIVATE
    CompilerConfig
    PrecompileStdHeaders
    TbFsLib
  PUBLIC
    TbBaseLib
    TbLitIdLib
    nvrhi
    nvrhi_vk
    WrapVulkanHeaders::WrapVulkanHeaders
)

# R1.1: AppleClang's libc++ has no floating-point std::from_chars; lit/NumberText.h uses
# fast_float's there, as KdLib does (cmake/Dependencies.cmake adds it for AppleClang)
if (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  target_link_libraries(TbLitLib
    PRIVATE
      FastFloat::fast_float
  )
endif()

# V.4, G11: the first-party mirrors of the engine's float arithmetic (lit/NumberText.h's
# lexerFloat, the projection, light and entity code) are compiled with the engine's float
# flags, as TbLitIdLib is: GCC (fast) and Clang (on, where FMA exists, as on arm64) would
# otherwise fuse a * b + c (mirror.json M-ID-BUILD-01)
if(NOT MSVC)
  target_compile_options(TbLitLib
    PRIVATE
      -ffp-contract=off
      $<$<NOT:$<CONFIG:Debug>>:-fno-math-errno>
      $<$<NOT:$<CONFIG:Debug>>:-fno-trapping-math>
  )
endif()

# TbLitIdLib is public for its std-only facade, lit/MaterialDecls.h, which
# lit/MaterialService.h and lit/GameFileSource.h include. Its shim headers (precompiled.h,
# RenderCommon.h) and every id type are private to it, so nothing else is passed on.

# Vulkan is reached only through vulkan.hpp's dynamic dispatcher and the loader is never
# linked (C9), so a call to a static prototype fails to compile. On Windows vulkan.h
# includes windows.h, whose min and max macros NOMINMAX keeps out.
target_compile_definitions(TbLitLib
  PUBLIC
    VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
    VK_NO_PROTOTYPES
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
)

# G12: TrenchBroom's Release link passes /DEBUG (app/TrenchBroom/CMakeLists.txt), which
# turns /OPT:REF off, so the copied Str.cpp's six thread_local buffers stay in although
# nothing calls their functions: 192 KB of static TLS in every thread (mirror.json
# M-ID-BUILD-01). /OPT:REF drops the unreferenced functions and their TLS, as every Release
# link without /DEBUG already does. An explicit /OPT:REF also folds identical functions
# (/OPT:ICF) unless /OPT:NOICF is given; W.8 gives it, so that no two functions share an
# address (cpptrace's frames from the stripped PDB, Qt's pointer-to-member comparisons).
if(MSVC)
  target_link_options(TbLitLib
    INTERFACE
      $<$<CONFIG:Release>:/OPT:REF>
      $<$<CONFIG:Release>:/OPT:NOICF>
  )
endif()

add_subdirectory(test)
