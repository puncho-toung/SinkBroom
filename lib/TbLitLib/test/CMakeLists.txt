add_executable(TbLitLibTest)
EMBED_UTF8_MANIFEST(TbLitLibTest)

target_sources(TbLitLibTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/LitMapFixture.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/LitMapFixture.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/LitTestUtils.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/LitTestUtils.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CpuPool.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_DeviceChoice.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EngineGeometry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_FrameLog.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_FramePacer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_GameDeclSource.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Geometry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_GeometryGoldens.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LitSession.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MaterialDecls.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MaterialService.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_NvrhiSupport.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_PersistentMap.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ResourceContainer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_RetailMaterials.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Snapshot.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SwapchainRules.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_VulkanInstance.cpp
)

add_compile_definitions(CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS=1)

# miniz writes the tests' .pk4 files
target_link_libraries(TbLitLibTest
  PRIVATE
    CompilerConfig
    PrecompileStdHeaders
    Catch2::Catch2WithMain
    TbBaseTestUtilsLib
    TbLitLib
    miniz
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
