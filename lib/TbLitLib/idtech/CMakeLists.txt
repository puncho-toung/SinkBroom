# TbLitIdLib: the engine's material parser, copied verbatim (engine/), with SinkBroom's shim
# and std-only facade (shim/). It is built like a dependency (D31, S18): C++17 as in the
# engine, the engine's semantic compile definitions, no CompilerConfig, no precompiled std
# headers and no TrenchBroom warnings. Only the facade header (shim/include) is visible to
# TbLitLib, which links this library PRIVATE.
add_library(TbLitIdLib STATIC)

set(TB_LIT_ID_ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine)
set(TB_LIT_ID_SHIM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shim)

target_sources(TbLitIdLib
  PRIVATE
    ${TB_LIT_ID_ENGINE_DIR}/framework/DeclEntityDef.cpp
    ${TB_LIT_ID_ENGINE_DIR}/framework/DeclSkin.cpp
    ${TB_LIT_ID_ENGINE_DIR}/framework/DeclTable.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/Dict.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/Lexer.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/Str.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/Token.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/containers/HashIndex.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/math/Math.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/math/Matrix.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/math/Vector.cpp
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Image_process.cpp
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Image_program.cpp
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Material.cpp
    # compiled inside shim/src/DeclManagerShim.cpp, which defines the members it leaves out
    ${TB_LIT_ID_ENGINE_DIR}/framework/DeclManager.cpp

    ${TB_LIT_ID_SHIM_DIR}/src/DeclManagerShim.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/MaterialDecls.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/Registry.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/ShimFramework.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/ShimLib.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/ShimRenderer.cpp
    ${TB_LIT_ID_SHIM_DIR}/private/precompiled.h
    ${TB_LIT_ID_SHIM_DIR}/private/RenderCommon.h
    ${TB_LIT_ID_SHIM_DIR}/private/shim/Registry.h
    ${TB_LIT_ID_SHIM_DIR}/private/shim/ShimEngine.h
    ${TB_LIT_ID_SHIM_DIR}/private/shim/ShimFramework.h
    ${TB_LIT_ID_SHIM_DIR}/private/shim/ShimLib.h
    ${TB_LIT_ID_SHIM_DIR}/private/shim/ShimSystem.h

  PUBLIC FILE_SET headers TYPE HEADERS
    BASE_DIRS ${TB_LIT_ID_SHIM_DIR}/include
    FILES ${TB_LIT_ID_SHIM_DIR}/include/lit/MaterialDecls.h
)

set_source_files_properties(
  ${TB_LIT_ID_ENGINE_DIR}/framework/DeclManager.cpp
  PROPERTIES HEADER_FILE_ONLY ON)

# the shim headers named precompiled.h and RenderCommon.h come first, so the copied files'
# includes find them; then the copied tree, whose paths below neo/ are kept
target_include_directories(TbLitIdLib
  PRIVATE
    ${TB_LIT_ID_SHIM_DIR}/private
    ${TB_LIT_ID_ENGINE_DIR}
)

set_target_properties(TbLitIdLib PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

# The engine's compile definitions that the copied files branch on (neo/CMakeLists.txt:
# USE_INTRINSICS_SSE is the engine's default on x86 and x86-64). DMAP is never defined:
# under DMAP the parser keeps no programs and handles missing images differently (M8). The
# full set and the ones deliberately left out are recorded in mirror.json.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86|x86)$")
  target_compile_definitions(TbLitIdLib PRIVATE USE_INTRINSICS_SSE)
endif()

if(MSVC)
  # Debug objects of the large copied files need more sections; Release keeps symbols for
  # crash reports through id frames (S18)
  target_compile_options(TbLitIdLib PRIVATE
    $<$<CONFIG:Debug>:/bigobj>
    $<$<CONFIG:Release>:/Z7>
  )
endif()

suppress_dependency_warnings(TbLitIdLib)
apply_sanitizer_options(TbLitIdLib)
