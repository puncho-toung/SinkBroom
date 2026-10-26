# TbLitIdLib: the engine's material parser and geometry code, copied verbatim (engine/),
# with SinkBroom's shim and std-only facades (shim/). It is built like a dependency (D31,
# S18): C++17 as in the engine, the engine's semantic compile definitions, no
# CompilerConfig, no precompiled std headers and no TrenchBroom warnings. Only the facade
# headers (shim/include) are visible to TbLitLib, which links this library PUBLIC for them.
add_library(TbLitIdLib STATIC)

set(TB_LIT_ID_ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine)
set(TB_LIT_ID_SHIM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shim)

target_sources(TbLitIdLib
  PRIVATE
    ${TB_LIT_ID_ENGINE_DIR}/framework/DeclEntityDef.cpp
    ${TB_LIT_ID_ENGINE_DIR}/framework/DeclSkin.cpp
    ${TB_LIT_ID_ENGINE_DIR}/framework/DeclTable.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/CmdArgs.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/Dict.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/Lexer.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/Str.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/Token.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/bv/Bounds.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/containers/HashIndex.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/geometry/DrawVert.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/geometry/JointTransform.cpp
    # includes "../ParallelJobList_JobHeaders.h", which resolves to the shim's stand-in
    # through the shim/private include folder (shim/ParallelJobList_JobHeaders.h)
    ${TB_LIT_ID_ENGINE_DIR}/idlib/geometry/RenderMatrix.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/geometry/Surface.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/geometry/Surface_Patch.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/geometry/Winding.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/math/Angles.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/math/Math.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/math/Matrix.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/math/Plane.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/math/Pluecker.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/math/Quat.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/math/Rotation.cpp
    ${TB_LIT_ID_ENGINE_DIR}/idlib/math/Vector.cpp
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Color/ColorSpace.cpp
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Image_process.cpp
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Image_program.cpp
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Material.cpp
    # compiled inside shim/src/DeclManagerShim.cpp, which defines the members it leaves out
    ${TB_LIT_ID_ENGINE_DIR}/framework/DeclManager.cpp
    # compiled inside shim/src/EngineGeometry.cpp, which calls its static functions
    ${TB_LIT_ID_ENGINE_DIR}/renderer/tr_trisurf.cpp
    # LP1c.2: the image loaders with the stb_image and tinyexr implementations the engine
    # compiles in this file (the engine-vendored libraries are D27's recorded exception),
    # compiled inside shim/src/ShimImageFiles.cpp, which names tinyexr's miniz apart
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Image_files.cpp
    ${TB_LIT_ID_ENGINE_DIR}/libs/mesa/format_r11g11b10f.h
    ${TB_LIT_ID_ENGINE_DIR}/libs/stb/stb_image.h
    ${TB_LIT_ID_ENGINE_DIR}/libs/tinyexr/tinyexr.h
    # LP2.0: GetRenderState and GetRowPitch with what they call (C24, P2.25), compiled
    # inside shim/src/ShimPipelineState.cpp, which declares the class they belong to
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Image_load.cpp
    ${TB_LIT_ID_ENGINE_DIR}/renderer/PipelineCache.cpp
    ${TB_LIT_ID_ENGINE_DIR}/renderer/PipelineCache.h
    # DX.2: R_CalcInteractionCullBits' and R_CreateInteractionLightTris' ranges, compiled
    # inside a function of shim/src/EngineInteraction.cpp, which declares what they name
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Interaction.cpp
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Interaction.h
    # LP4.2a (D85): the BRDF lookup table's data header, copied whole, included by
    # shim/src/EngineImageData.cpp behind the facade lit/EngineImageData.h
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Image_brdfLut.h
    # LP7.1a (C51, D135, D137): the OBJ loader, copied whole; renderer/Model.cpp's ranges,
    # compiled inside shim/src/ShimModels.cpp, which declares the reduced
    # idRenderModelStatic they belong to; the conversion's slop merge (VectorSet.h);
    # idSIMD_Generic's two MinMax ranges, compiled inside shim/src/ShimLib.cpp
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Model_obj.cpp
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Model_obj.h
    # LP7.1b (C51, P7.13): the ASE loader, copied whole; renderer/Model.cpp's ASE range
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Model_ase.cpp
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Model_ase.h
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Model.cpp
    ${TB_LIT_ID_ENGINE_DIR}/renderer/Model.h
    ${TB_LIT_ID_ENGINE_DIR}/idlib/containers/VectorSet.h
    ${TB_LIT_ID_ENGINE_DIR}/idlib/math/Simd.h
    ${TB_LIT_ID_ENGINE_DIR}/idlib/math/Simd_Generic.cpp
    # LP7.1a (D135): the engine-vendored MikkTSpace, copied whole as a pinned library
    # (zlib-style licence, LICENSE_EXCEPTIONS.md), compiled as C with this target's flags;
    # tr_trisurf.cpp's R_DeriveMikktspaceTangents calls it
    ${TB_LIT_ID_ENGINE_DIR}/libs/mikktspace/mikktspace.c
    ${TB_LIT_ID_ENGINE_DIR}/libs/mikktspace/mikktspace.h

    ${TB_LIT_ID_SHIM_DIR}/src/DeclManagerShim.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/EngineGeometry.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/EngineImageData.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/EngineInteraction.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/EngineLight.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/EngineLexer.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/EngineMessages.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/ImageFileCheck.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/ShimImageFiles.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/ShimImageSource.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/MaterialDecls.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/Registry.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/ShimFramework.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/ShimImages.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/ShimLib.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/ShimModels.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/ShimPipelineState.cpp
    ${TB_LIT_ID_SHIM_DIR}/src/ShimRenderer.cpp
    ${TB_LIT_ID_SHIM_DIR}/ParallelJobList_JobHeaders.h
    ${TB_LIT_ID_SHIM_DIR}/private/precompiled.h
    ${TB_LIT_ID_SHIM_DIR}/private/RenderCommon.h
    ${TB_LIT_ID_SHIM_DIR}/private/shim/EngineMessages.h
    ${TB_LIT_ID_SHIM_DIR}/private/shim/Registry.h
    ${TB_LIT_ID_SHIM_DIR}/private/shim/ShimEngine.h
    ${TB_LIT_ID_SHIM_DIR}/private/shim/ShimFramework.h
    ${TB_LIT_ID_SHIM_DIR}/private/shim/ShimInteraction.h
    ${TB_LIT_ID_SHIM_DIR}/private/shim/ShimLib.h
    ${TB_LIT_ID_SHIM_DIR}/private/shim/ShimSystem.h
    ${TB_LIT_ID_SHIM_DIR}/private/shim/TinyexrMinizNames.h

  PUBLIC FILE_SET headers TYPE HEADERS
    BASE_DIRS ${TB_LIT_ID_SHIM_DIR}/include
    FILES
      ${TB_LIT_ID_SHIM_DIR}/include/lit/EngineBuiltins.h
      ${TB_LIT_ID_SHIM_DIR}/include/lit/EngineGeometry.h
      ${TB_LIT_ID_SHIM_DIR}/include/lit/EngineImageData.h
      ${TB_LIT_ID_SHIM_DIR}/include/lit/EngineImageEnums.h
      ${TB_LIT_ID_SHIM_DIR}/include/lit/EngineLexer.h
      ${TB_LIT_ID_SHIM_DIR}/include/lit/EngineLight.h
      ${TB_LIT_ID_SHIM_DIR}/include/lit/EngineModels.h
      ${TB_LIT_ID_SHIM_DIR}/include/lit/EnginePipelineState.h
      ${TB_LIT_ID_SHIM_DIR}/include/lit/MaterialDecls.h
)

set_source_files_properties(
  ${TB_LIT_ID_ENGINE_DIR}/framework/DeclManager.cpp
  ${TB_LIT_ID_ENGINE_DIR}/renderer/tr_trisurf.cpp
  ${TB_LIT_ID_ENGINE_DIR}/renderer/Image_files.cpp
  ${TB_LIT_ID_ENGINE_DIR}/renderer/Image_load.cpp
  ${TB_LIT_ID_ENGINE_DIR}/renderer/PipelineCache.cpp
  ${TB_LIT_ID_ENGINE_DIR}/renderer/Interaction.cpp
  ${TB_LIT_ID_ENGINE_DIR}/renderer/Model.cpp
  ${TB_LIT_ID_ENGINE_DIR}/idlib/math/Simd_Generic.cpp
  PROPERTIES HEADER_FILE_ONLY ON)

# the shim headers named precompiled.h and RenderCommon.h come first, so the copied files'
# includes find them; then the copied tree, whose paths below neo/ are kept
target_include_directories(TbLitIdLib
  PRIVATE
    ${TB_LIT_ID_SHIM_DIR}/private
    ${TB_LIT_ID_ENGINE_DIR}
    # LP7.1a: tr_trisurf.cpp includes <mikktspace.h>, as the engine's build finds it
    ${TB_LIT_ID_ENGINE_DIR}/libs/mikktspace
)

# LP2.0: the facade lit/EnginePipelineState.h speaks NVRHI's render-state types, and the
# copied PipelineCache.cpp builds them; NVRHI's headers only, its code is TbLitLib's
target_link_libraries(TbLitIdLib PUBLIC nvrhi)

set_target_properties(TbLitIdLib PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

# The engine's compile definitions that the copied files branch on (neo/CMakeLists.txt:
# USE_INTRINSICS_SSE is the engine option's default on x86 and x86-64, unless an Apple
# build targets arm64, neo/CMakeLists.txt:88-93). DMAP is never defined: under DMAP the
# parser keeps no programs and handles missing images differently (M8). The full set and
# the ones deliberately left out are recorded in mirror.json (M-ID-BUILD-01).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)|(e2k)|(E2K)"
   AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
  target_compile_definitions(TbLitIdLib PRIVATE USE_INTRINSICS_SSE)
endif()

# G11, M-ID-BUILD-01: the engine's GCC and Clang code-generation flags. It compiles
# everything with -fno-strict-aliasing (neo/CMakeLists.txt:199), which the copied idMath's
# type punning needs, and its optimised configurations with -ffp-contract=off
# -fno-math-errno -fno-trapping-math (:220-222). -ffp-contract=off applies to every
# configuration here, so that Debug evaluates registers as Release does. The engine's
# GCC and Clang Debug defines _DEBUG (:217), as /MDd does on MSVC, so the copied asserts
# are live in every Debug build.
if(NOT MSVC)
  target_compile_options(TbLitIdLib PRIVATE
    -fno-strict-aliasing
    -ffp-contract=off
    $<$<NOT:$<CONFIG:Debug>>:-fno-math-errno>
    $<$<NOT:$<CONFIG:Debug>>:-fno-trapping-math>
  )
  target_compile_definitions(TbLitIdLib PRIVATE $<$<CONFIG:Debug>:_DEBUG>)
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

# LP1c fix (S3.2): the engine's fast DXT encoders, copied verbatim, for the image goldens'
# exact comparison with the engine's caches. Test only: linked by TbLitLibTest alone, never
# by the product, which encodes no BC on the CPU (D24). Built as TbLitIdLib is built.
add_library(TbLitIdDxtTestLib STATIC)

target_sources(TbLitIdDxtTestLib
  PRIVATE
    ${TB_LIT_ID_ENGINE_DIR}/renderer/DXT/DXTCodec.h
    ${TB_LIT_ID_ENGINE_DIR}/renderer/DXT/DXTCodec_local.h
    ${TB_LIT_ID_ENGINE_DIR}/renderer/DXT/DXTEncoder_SSE2.cpp
    ${TB_LIT_ID_SHIM_DIR}/testing/src/DxtEncoderForTesting.cpp

  PUBLIC FILE_SET headers TYPE HEADERS
    BASE_DIRS ${TB_LIT_ID_SHIM_DIR}/testing/include
    FILES
      ${TB_LIT_ID_SHIM_DIR}/testing/include/lit/DxtEncoderForTesting.h
)

target_include_directories(TbLitIdDxtTestLib
  PRIVATE
    ${TB_LIT_ID_SHIM_DIR}/private
    ${TB_LIT_ID_ENGINE_DIR}
)

target_link_libraries(TbLitIdDxtTestLib PRIVATE TbLitIdLib)

set_target_properties(TbLitIdDxtTestLib PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)|(e2k)|(E2K)"
   AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
  target_compile_definitions(TbLitIdDxtTestLib PRIVATE USE_INTRINSICS_SSE)
endif()

if(NOT MSVC)
  target_compile_options(TbLitIdDxtTestLib PRIVATE
    -fno-strict-aliasing
    -ffp-contract=off
    $<$<NOT:$<CONFIG:Debug>>:-fno-math-errno>
    $<$<NOT:$<CONFIG:Debug>>:-fno-trapping-math>
  )
  target_compile_definitions(TbLitIdDxtTestLib PRIVATE $<$<CONFIG:Debug>:_DEBUG>)
endif()

suppress_dependency_warnings(TbLitIdDxtTestLib)
apply_sanitizer_options(TbLitIdDxtTestLib)
