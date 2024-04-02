# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: MPL-2.0
#
#[=======================================================================[

  CMake Configuration for VDB View

#]=======================================================================]

cmake_minimum_required(VERSION 3.18)
project(VkVDBView LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include("ShaderCompilation")

# Enforce use of C++17
set(CMAKE_CXX_STANDARD_REQUIRED 17)
set(CMAKE_CXX_STANDARD 17)

# Find graphics libraries 
find_package(OpenGL REQUIRED)
find_package(Vulkan REQUIRED)
find_package(VulkanMemoryAllocator CONFIG)

# The VMA often cannot be found through Cmake, since it is a single header library 
# usually packaged without a CMake config. Here we verify it can be included despite
# not being found. 
if (NOT "${VulkanMemoryAllocator_FOUND}")
  include(CheckIncludeFileCXX)

  # Usually the VMA is packaged with the VulkanSDK or with the other installed Vulkan headers
  # Make sure the Vulkan package is used for the include file test. 
  set(CMAKE_REQUIRED_LIBRARIES Vulkan::Vulkan)
  CHECK_INCLUDE_FILE_CXX("vma/vk_mem_alloc.h" VDB_VIEW_VMA_INCLUDE_CONFIRMED)

  if (NOT "${VDB_VIEW_VMA_INCLUDE_CONFIRMED}")
    message(FATAL_ERROR "Couldn't find VulkanMemoryAllocator (VMA) through CMake find_package() or with the other Vulkan headers.")
  else()
    message(STATUS "Found VulkanMemoryAllocator (VMA) through Vulkan package or system includes.")
  endif()

  unset(CMAKE_REQUIRED_LIBRARIES)
endif()

# wraps find_package(glfw3) and sets the glfw target
include(OpenVDBGLFW3Setup)


set(SOURCE_FILES
  "${PROJECT_SOURCE_DIR}/src/main.cc"
  "${PROJECT_SOURCE_DIR}/src/Camera.cc"
  "${PROJECT_SOURCE_DIR}/src/ClipBox.cc"
  "${PROJECT_SOURCE_DIR}/src/Font.cc"
  "${PROJECT_SOURCE_DIR}/src/RenderModules.cc"
  "${PROJECT_SOURCE_DIR}/src/Viewer.cc"
  "${PROJECT_SOURCE_DIR}/src/vulkan/Utils.cc"
  "${PROJECT_SOURCE_DIR}/src/vulkan/GlfwVulkan.cc"
  "${PROJECT_SOURCE_DIR}/src/vulkan/Resources.cc"
  "${PROJECT_SOURCE_DIR}/src/vulkan/ClassicRaster.cc"
  "${PROJECT_SOURCE_DIR}/src/vulkan/BitmapFont.cc"
  )

file(GLOB_RECURSE HEADER_FILES "${PROJECT_SOURCE_DIR}/src/*.h" "${PROJECT_SOURCE_DIR}/src/*.hpp")
set(VDB_VIEW_EXEC_TARGET "vk_vdb_view")
add_executable(${VDB_VIEW_EXEC_TARGET} ${SOURCE_FILES} ${HEADER_FILES})

target_link_libraries(${VDB_VIEW_EXEC_TARGET}
  ${OPENVDB_BINARIES_DEPENDENT_LIBS}
  Vulkan::Vulkan
  OpenGL::GL
  OpenGL::GLU
  glfw
  ${CMAKE_DL_LIBS}
)

if (GPUOpen::VulkanMemoryAllocator) 
  target_link_libraries(${VDB_VIEW_EXEC_TARGET} GPUOpen::VulkanMemoryAllocator)
endif()

target_compile_definitions(${VDB_VIEW_EXEC_TARGET} PRIVATE -DGL_GLEXT_PROTOTYPES=1)

target_add_glsl_shaders_cpp(${VDB_VIEW_EXEC_TARGET}
  SOURCES
    "${PROJECT_SOURCE_DIR}/src/glsl/standard.vert"
    "${PROJECT_SOURCE_DIR}/src/glsl/standard.frag"
    "${PROJECT_SOURCE_DIR}/src/glsl/bitmap.vert"
    "${PROJECT_SOURCE_DIR}/src/glsl/bitmap.frag"
  IDENTIFIERS
    "sStandardVertexShader"
    "sStandardFragmentShader"
    "sBitmapVertexShader"
    "sBitmapFragmentShader"
)

# Configure Installation Package
###############################################################
include(GNUInstallDirs)

install(TARGETS ${VDB_VIEW_EXEC_TARGET} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})