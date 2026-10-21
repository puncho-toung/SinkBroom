add_executable(TbLitLibTest)
EMBED_UTF8_MANIFEST(TbLitLibTest)

target_sources(TbLitLibTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/LitTestUtils.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/LitTestUtils.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_DeviceChoice.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_FrameLog.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_FramePacer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_GameDeclSource.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LitSession.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MaterialDecls.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MaterialService.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_NvrhiSupport.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_RetailMaterials.cpp
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

# the parse thread gets the executable's stack on Windows; the tests run it with
# TrenchBroom's 8 MB (app/TrenchBroom/CMakeLists.txt)
if(MSVC)
  target_link_options(TbLitLibTest PRIVATE "/STACK:8388608")
endif()

# Copy fixtures
set(TEST_FIXTURE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fixture)
set(TEST_FIXTURE_DEST_DIR $<TARGET_FILE_DIR:TbLitLibTest>/fixture)

add_custom_command(TARGET TbLitLibTest POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${TEST_FIXTURE_DEST_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TEST_FIXTURE_SOURCE_DIR}" "${TEST_FIXTURE_DEST_DIR}/test"
)

include(Catch)
catch_discover_tests(TbLitLibTest)
