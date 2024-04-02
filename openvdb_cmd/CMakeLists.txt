## Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: MPL-2.0
#
#[=======================================================================[

  CMake Configuration for OpenVDB Binaries

#]=======================================================================]

cmake_minimum_required(VERSION 3.18)
project(OpenVDBBinaries LANGUAGES CXX)

include(GNUInstallDirs)

###### OpenVDB Binary Component Options

option(OPENVDB_BUILD_VDB_PRINT "Build vdb_print" ON)
option(OPENVDB_BUILD_VDB_LOD "Build vdb_lod" OFF)
option(OPENVDB_BUILD_VDB_RENDER "Build vdb_render" OFF)
option(OPENVDB_BUILD_VDB_VIEW "Build vdb_view" OFF)
option(OPENVDB_BUILD_VK_VDB_VIEW "Build Vulkan vdb_view" OFF)
option(OPENVDB_BUILD_VDB_TOOL "Build vdb_tool" OFF)
option(OPENVDB_BUILD_VDB_AX "Build the OpenVDB AX command line binary" OFF)

if(OPENVDB_BUILD_AX_BINARIES)
  message(DEPRECATION "OPENVDB_BUILD_AX_BINARIES is deprecated. Use OPENVDB_BUILD_VDB_AX.")
  set(OPENVDB_BUILD_VDB_AX ON)
endif()

#########################################################################

message(STATUS "----------------------------------------------------")
message(STATUS "----------- Configuring OpenVDBBinaries ------------")
message(STATUS "----------------------------------------------------")

##########################################################################

# Collect lib dependencies shared by all binaries

if(NOT OPENVDB_BUILD_CORE)
  # @note  Could also use the openvdb_je target here, but we just opt to
  # handle the value of CONCURRENT_MALLOC outside of this branching for
  # both cases
  set(OPENVDB_LIB OpenVDB::openvdb)
else()
  set(OPENVDB_LIB openvdb)
endif()

set(OPENVDB_BINARIES_DEPENDENT_LIBS
  ${OPENVDB_LIB}
)

if(CONCURRENT_MALLOC STREQUAL "Jemalloc")
  find_package(Jemalloc REQUIRED)
  list(APPEND OPENVDB_BINARIES_DEPENDENT_LIBS Jemalloc::jemalloc)
elseif(CONCURRENT_MALLOC STREQUAL "Tbbmalloc")
  find_package(TBB ${MINIMUM_TBB_VERSION} REQUIRED COMPONENTS tbbmalloc)
  list(APPEND OPENVDB_BINARIES_DEPENDENT_LIBS TBB::tbbmalloc)
endif()

##########################################################################

if (OPENVDB_BUILD_VDB_PRINT)
  add_subdirectory(vdb_print)
endif()

if (OPENVDB_BUILD_VDB_LOD)
  add_subdirectory(vdb_lod)
endif()

if (OPENVDB_BUILD_VDB_RENDER)
  add_subdirectory(vdb_render)
endif()

if (OPENVDB_BUILD_VDB_TOOL)
  add_subdirectory(vdb_tool)
endif()

if (OPENVDB_BUILD_VDB_VIEW)
  add_subdirectory(vdb_view)
endif()

if (OPENVDB_BUILD_VK_VDB_VIEW)
  add_subdirectory(vk_vdb_view)
endif()

if (OPENVDB_BUILD_VDB_AX)
  add_subdirectory(vdb_ax)
endif()
