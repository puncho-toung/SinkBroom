# SPIRV-Reflect, for the lit preview (TB_ENABLE_LIT_PREVIEW): every engine shader blob the
# preview loads is reflected and compared with the mirrored binding layouts and renderparm
# sets before a pipeline is made from it (D9, C26). Pinned to a tagged release of the
# Vulkan SDK line of the pinned Vulkan headers (cmake/dependencies/nvrhi.cmake); a
# SinkBroom pin, the engine has no SPIRV-Reflect (mirror.json "pins").
#
# Download only: its own CMake builds the spirv-reflect executable and its tests. Of it
# only spirv_reflect.c is built, as the static library SpirvReflect, with the SPIR-V
# header it ships (include/spirv/unified1/spirv.h). It needs no Vulkan header, so the one
# Vulkan header set (C13) and VK_NO_PROTOTYPES (C9) stand.
#
# T0.11: by the commit of the tag vulkan-sdk-1.4.304.0 (a tag can move), without its
# third_party/googletest submodule. V2.9 (32-LP2-VERIFY.md): CPM drops the empty value
# CMP0097 reads as "none" (ExternalProject gets GIT_SUBMODULES with no value and clones
# every submodule), so the list names a folder of the repository that holds no
# submodule: `git submodule update --init include` initialises none (checked with an
# empty CPM cache)
CPMAddPackage(
  NAME SPIRV-Reflect
  GITHUB_REPOSITORY KhronosGroup/SPIRV-Reflect
  GIT_TAG 281b55edb67b1f2e61e1dc92fdc42a650da05d83
  GIT_SUBMODULES "include"
  DOWNLOAD_ONLY YES
)

add_library(SpirvReflect STATIC
  "${SPIRV-Reflect_SOURCE_DIR}/spirv_reflect.c"
  "${SPIRV-Reflect_SOURCE_DIR}/spirv_reflect.h"
)
# its headers are system headers for SinkBroom's code, which compiles with TrenchBroom's
# warnings
target_include_directories(SpirvReflect PRIVATE "${SPIRV-Reflect_SOURCE_DIR}")
target_include_directories(SpirvReflect SYSTEM INTERFACE "${SPIRV-Reflect_SOURCE_DIR}")
# spirv_reflect.h's C++ wrapper (spv_reflect::ShaderModule) is not used
target_compile_definitions(SpirvReflect
  PUBLIC SPIRV_REFLECT_DISABLE_CPP_BINDINGS
)

suppress_dependency_warnings(SpirvReflect)
apply_sanitizer_options(SpirvReflect)
