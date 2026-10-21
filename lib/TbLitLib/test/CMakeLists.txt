add_executable(TbLitLibTest)
EMBED_UTF8_MANIFEST(TbLitLibTest)

target_sources(TbLitLibTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_DeviceChoice.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_FrameLog.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_FramePacing.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LitConfig.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LitSession.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_NvrhiSupport.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SwapchainRules.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_VulkanInstance.cpp
)

add_compile_definitions(CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS=1)

target_link_libraries(TbLitLibTest
  PRIVATE
    CompilerConfig
    PrecompileStdHeaders
    Catch2::Catch2WithMain
    TbLitLib
)

include(Catch)
catch_discover_tests(TbLitLibTest)
