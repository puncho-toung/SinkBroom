# ShaderMake's blob reader, for the lit preview (TB_ENABLE_LIT_PREVIEW): the engine's
# renderprogs2/spirv/*.bin files are ShaderMake permutation blobs, and the preview finds a
# permutation in them with ShaderMake's own FindPermutationInBlob (C2, C26). It is pinned
# to the commit of the engine's neo/extern/ShaderMake gitlink, the RobertBeckebans fork.
#
# Download only: ShaderMake's own CMake builds the ShaderMake executable and looks for
# DXC. Of it only src/ShaderBlob.cpp and include/ShaderMake/ShaderBlob.h are built, as the
# static library ShaderBlob, which needs nothing but the C++ standard library. Its two
# globals (the "NVSP" signature and its size) are constant-initialised (C9).
CPMAddPackage(
  NAME ShaderMake
  GITHUB_REPOSITORY RobertBeckebans/ShaderMake
  GIT_TAG 642610088c7e1fc940665e7c482e36e0d7b633c8
  DOWNLOAD_ONLY YES
)

add_library(ShaderBlob STATIC
  "${ShaderMake_SOURCE_DIR}/src/ShaderBlob.cpp"
  "${ShaderMake_SOURCE_DIR}/include/ShaderMake/ShaderBlob.h"
)
# its headers are system headers for SinkBroom's code, which compiles with TrenchBroom's
# warnings
target_include_directories(ShaderBlob PRIVATE "${ShaderMake_SOURCE_DIR}/include")
target_include_directories(ShaderBlob SYSTEM INTERFACE "${ShaderMake_SOURCE_DIR}/include")
set_target_properties(ShaderBlob PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

suppress_dependency_warnings(ShaderBlob)
apply_sanitizer_options(ShaderBlob)

# The blob format packs each entry's 8-byte header after a key of any length, so
# FindPermutationInBlob reads the headers unaligned, which UBSan reports. SinkBroom's own
# walks of a blob read each header through an aligned copy (lit/ShaderBlobs.h); the
# reader's own lookup is kept as it is (P2.22).
if(TB_ENABLE_UBSAN AND NOT MSVC)
  target_compile_options(ShaderBlob PRIVATE -fno-sanitize=alignment)
endif()
