add_executable(TbLitLibTest)
EMBED_UTF8_MANIFEST(TbLitLibTest)

target_sources(TbLitLibTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ImageTestFiles.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ImageTestFiles.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/LitMapFixture.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/LitMapFixture.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/LitTestUtils.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/LitTestUtils.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/TonemapModel.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/TonemapModel.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_AmbientPass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_AreaData.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CpuPool.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_DecalProjectors.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_DeviceChoice.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EditorImageSize.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EngineGeometry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EngineLight.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EngineTables.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Feed.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_FeedScene.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_FrameLog.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_FramePacer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_GameFile.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_GameFileSource.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Geometry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_GeometryGoldens.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_GridTable.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ImageCache.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ImageFallback.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ImageFiles.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ImageGoldens.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ImageOverride.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ImageSlots.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_IntrinsicImages.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LightData.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LightGoldens.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LightParse.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LightRecord.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LightTriangles.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LitPasses.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LitSession.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LitStagePass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LitTestUtils.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LitView.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MapBake.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MaterialDecls.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MaterialService.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ModelEntities.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ModelLoading.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_NumberText.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_NvrhiSupport.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_PersistentMap.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ProbeRecord.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ProcIslands.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Projection.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_RangeAllocator.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ResourceContainer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_RetailMaterials.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_RetailModels.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_RetailShaders.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ShaderBlobs.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ShaderReflection.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Snapshot.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_StackThread.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SwapchainRules.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_TextureUploads.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_TonemapPasses.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ViewFlow.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ViewProbes.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_VulkanInstance.cpp
)

add_compile_definitions(CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS=1)

# miniz writes the tests' .pk4 files; fs::TestEnvironment makes their game folders (A1.11)
target_link_libraries(TbLitLibTest
  PRIVATE
    CompilerConfig
    PrecompileStdHeaders
    Catch2::Catch2WithMain
    TbBaseTestUtilsLib
    TbFsTestUtilsLib
    TbLitLib
    # LP1c fix (S3.2): the engine's fast DXT encoders, for the tests only
    TbLitIdDxtTestLib
    miniz
)

# A1.11: the private StackThread is tested directly
target_include_directories(TbLitLibTest
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# Copy fixtures
set(TEST_FIXTURE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fixture)
set(TEST_FIXTURE_DEST_DIR $<TARGET_FILE_DIR:TbLitLibTest>/fixture)

add_custom_command(TARGET TbLitLibTest POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${TEST_FIXTURE_DEST_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TEST_FIXTURE_SOURCE_DIR}" "${TEST_FIXTURE_DEST_DIR}/test"
)

include(Catch)
catch_discover_tests(TbLitLibTest)
