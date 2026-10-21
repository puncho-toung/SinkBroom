add_executable(TbUiLibTest)
EMBED_UTF8_MANIFEST(TbUiLibTest)

target_sources(TbUiLibTest
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RunAllTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SidebarTestUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SidebarTestUtils.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ActionContext.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ActionInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Actions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CellView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CompilationDialog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CompilationProfileManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CompilationRunner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CompilationTaskListBox.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Console.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ViewEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EntityDefinitionFileChooser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EntityInspector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EntityPropertyModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_FaceInspector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_GamesPreferencePane.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_InputEvent.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_IssueBrowser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LaunchGameEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LightEditorPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MapInspector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MapViewToolBox.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MapWindow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MapWindowManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MaterialBrowserView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MaterialCollectionEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_PrefabPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_QPathUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_QPreferenceStore.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_QPreferenceStoreUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_QStringUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_QVecUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_RecentDocuments.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SelectionHitchBenchmark.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SidebarManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SidebarPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SidebarPanels.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_KeyboardShortcutUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LayerListBox.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SystemPaths.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_TabBar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_TextOutputAdapter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ToolSidebar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_UpdateVersion.cpp

  PUBLIC FILE_SET cmd_tool TYPE HEADERS
    BASE_DIRS ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>
)

add_compile_definitions(CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS=1)

target_link_libraries(TbUiLibTest PRIVATE 
  CompilerConfig
  PrecompileQtHeaders
  PrecompileStdHeaders
  Catch2::Catch2WithMain
  Qt6::Network
  Qt6::OpenGL
  Qt6::OpenGLWidgets
  Qt6::Svg
  Qt6::Test
  Qt6::Widgets
  TbAppTestUtilsLib
  TbBaseTestUtilsLib
  TbElLib
  TbElTestUtilsLib
  TbFsTestUtilsLib
  TbMdlTestUtilsLib
  TbPreferencesLib
  TbRenderLib
  TbUiLib
  TbUiTestUtilsLib
  UpdateLib
)

if(TB_ENABLE_LIT_PREVIEW)
  # TbUiLib's definition is private; the tests of the sidebar docks test their own. C9:
  # the test target links no Vulkan library either.
  target_compile_definitions(TbUiLibTest PRIVATE TB_ENABLE_LIT_PREVIEW=1)
endif()

set_target_properties(TbUiLibTest PROPERTIES AUTOMOC TRUE)

add_dependencies(TbUiLibTest CmdTool)
file(GENERATE
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/CmdTool.h"
  CONTENT "#define CMD_TOOL_PATH \"$<TARGET_FILE:CmdTool>\"")

# Copy fixtures
set(TEST_FIXTURE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fixture)
set(TEST_FIXTURE_DEST_DIR $<TARGET_FILE_DIR:TbUiLibTest>/fixture)

add_custom_command(TARGET TbUiLibTest POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${TEST_FIXTURE_DEST_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TEST_FIXTURE_SOURCE_DIR}" "${TEST_FIXTURE_DEST_DIR}/test"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../../TbMdlLib/test/fixture/mdl/Game/Doom3BFG" "${TEST_FIXTURE_DEST_DIR}/test/mdl/Game/Doom3BFG"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/games" "${TEST_FIXTURE_DEST_DIR}/games"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/games-testing" "${TEST_FIXTURE_DEST_DIR}/games"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/graphics/images" "$<TARGET_FILE_DIR:TbUiLibTest>/images"
  # the GL views of the selection hitch benchmark load the shaders and the fonts
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/shader" "$<TARGET_FILE_DIR:TbUiLibTest>/shader"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/fonts" "$<TARGET_FILE_DIR:TbUiLibTest>/fonts"
)

include(Catch)
catch_discover_tests(TbUiLibTest)
