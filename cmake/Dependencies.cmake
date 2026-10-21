include(cmake/CPM.cmake)

include(cmake/dependencies/CompilerConfig.cmake)

include(cmake/dependencies/assimp.cmake)
include(cmake/dependencies/catch2.cmake)
include(cmake/dependencies/cpptrace.cmake)
include(cmake/dependencies/ctre.cmake)
include(cmake/dependencies/fmt.cmake)
include(cmake/dependencies/freeimage.cmake)
include(cmake/dependencies/freetype.cmake)
include(cmake/dependencies/miniz.cmake)
include(cmake/dependencies/stduuid.cmake)
include(cmake/dependencies/tinyxml2.cmake)

# The lit preview, a Vulkan view that draws the map with the engine's shaders through NVRHI,
# is opt-in. Unset means off, as for the other TB_ENABLE_* switches; with it off, nothing of
# it is fetched or built. NVRHI must come before find_package(Qt6) because it pins the
# Vulkan headers that Qt picks up there.
if(TB_ENABLE_LIT_PREVIEW)
  message(STATUS "Enabling the lit preview")
  if(NOT WIN32)
    message(WARNING "The lit preview is only tested on Windows")
  endif()
  include(cmake/dependencies/nvrhi.cmake)
endif()

# Find Qt and OpenGL
find_package(OpenGL REQUIRED)
find_package(Qt6 COMPONENTS Core Widgets OpenGL OpenGLWidgets Network Svg Test REQUIRED)

# The lit preview's window is a Vulkan QWindow, which needs Qt's vulkan feature.
if(TB_ENABLE_LIT_PREVIEW AND NOT QT_FEATURE_vulkan)
  message(FATAL_ERROR
    "TB_ENABLE_LIT_PREVIEW needs a Qt built with the vulkan feature, which this Qt lacks")
endif()

if(DEFINED Qt6_DIR)
  get_filename_component(TB_QT_LIBRARY_DIR "${Qt6_DIR}/../.." ABSOLUTE)
  message(STATUS "Found Qt6: ${TB_QT_LIBRARY_DIR}")
endif()

# Find threads lib, needed to work around a gtest bug, see: https://stackoverflow.com/questions/21116622/undefined-reference-to-pthread-key-create-linker-error
# The googletest target links to this
find_package(Threads)

# Use the fast-float library for from_chars on AppleClang, where std::from_chars is not
# available for floating point types.
if (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
include(cmake/dependencies/fast-float.cmake)
endif()
