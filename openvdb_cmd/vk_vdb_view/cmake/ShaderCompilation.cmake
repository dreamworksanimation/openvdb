# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.18)

# Runs when this CMake file is invoked as a script by the final custom command target created by `target_add_glsl_shader_cpp()`.
# Completes the work of `target_add_glsl_shader_cpp()` by taking the C-array SPIR-V bytecode generated previously and using them
# to generate a C++ source file.
if (CMAKE_SCRIPT_MODE_FILE MATCHES ${CMAKE_CURRENT_LIST_FILE})
    # Gather arguments and parse
    set(svargs DESTINATION_HEADER DESTINATION_SOURCE)
    set(mvargs INPUTS IDS)
    set(args "")
    foreach(x RANGE 4 ${CMAKE_ARGC})
        list(APPEND args ${CMAKE_ARGV${x}})
    endforeach()
    cmake_parse_arguments("SHADER_COMPILE_SCRIPTOOL" "" "${svargs}" "${mvargs}" ${args})

    cmake_path(GET SHADER_COMPILE_SCRIPTOOL_DESTINATION_HEADER FILENAME header_basename)
    file(WRITE "${SHADER_COMPILE_SCRIPTOOL_DESTINATION_SOURCE}" "// Automatically generated C++ code. Do not edit\n\n")
    file(APPEND "${SHADER_COMPILE_SCRIPTOOL_DESTINATION_SOURCE}" "#include \"${header_basename}\"\n#include <array>\n\n")

    foreach(input id IN ZIP_LISTS SHADER_COMPILE_SCRIPTOOL_INPUTS SHADER_COMPILE_SCRIPTOOL_IDS)
        file(READ "${input}" contents) # Read SPIR-V compiled GLSL encoded as comma separated list of 32-bit hex values
        
        # String manipulation to reformat text with 12 word lines and correct indentation
        set(r "0x[0-9a-f]+,")
        string(REPLACE "\n" "" contents "${contents}")
        string(REGEX REPLACE "${r}${r}${r}${r}${r}${r}${r}${r}${r}${r}${r}${r}" "\\0\n" contents "${contents}")
        string(REPLACE "\n" "\n    " contents "${contents}")

        # Append static bytecode arrays and class definition making it accessible through a global constant.
        file(APPEND "${SHADER_COMPILE_SCRIPTOOL_DESTINATION_SOURCE}" "static const uint32_t ${id}Bytecode[] = {\n    ${contents}\n};\n")
        file(APPEND "${SHADER_COMPILE_SCRIPTOOL_DESTINATION_SOURCE}" "const SpirVShaderBytecode ${id} {${id}Bytecode, std::size(${id}Bytecode)};\n\n")
    endforeach()
    return()
endif()

find_package(Vulkan REQUIRED)

set(COMPILED_GLSL_DIR "${CMAKE_BINARY_DIR}/shaders" CACHE PATH "Default location where GLSL shaders compiled into SPIR-V are written.")

if(Vulkan_VERSION)
    string(REGEX MATCH "[0-9]+\.[0-9]+" major_minor ${Vulkan_VERSION})
    set(target_env "vulkan${major_minor}")
    set(GLSLC_TARGET_ENV "${target_env}" CACHE STRING "Target environment identifier passed to GLSL compiler. Defaults to that matching the version of the found Vulkan SDK.")
else()
    set(GLSLC_TARGET_ENV "vulkan" CACHE STRING "Target environment identifier passed to GLSL compiler.")
endif()

option(OVERRIDE_GLSL_SHADER_DESTINATION_SUBDIR_SAFETY "Disable safety check preventing creation of GLSL shader output directory outside of CMake build directory" OFF)

set(GLSLC_FLAGS "--target-env=${GLSLC_TARGET_ENV}" "-x" "glsl")
set(GLSLC_FLAGS_DEBUG "-g" "-O0")
set(GLSLC_FLAGS_RELEASE "-O")
set(GLSLC_FLAGS_MINSIZEREL "-Os")
set(GLSLC_FLAGS_RELWITHDEBINFO "-g" "-O")


if (NOT Vulkan_GLSLC_EXECUTABLE) 
    message(FATAL_ERROR "GLSL compiler 'glslc' not found by CMake. Update your paths and try again or manually set 'Vulkan_GLSLC_EXECUTABLE'")
else()
    message(STATUS "Using GLSL compiler: '${Vulkan_GLSLC_EXECUTABLE}'")
endif()

function(target_add_glsl_shaders_cpp target)
    set(svargs DESTINATION_HEADER DESTINATION_SOURCE NAMESPACE)
    set(mvargs SOURCES IDENTIFIERS)
    cmake_parse_arguments(PARSE_ARGV 1 "ADD_GLSL_SHADER_CPP" "" "${svargs}" "${mvargs}")

    if(NOT ADD_GLSL_SHADER_CPP_DESTINATION_HEADER)
        set(ADD_GLSL_SHADER_CPP_DESTINATION_HEADER "${COMPILED_GLSL_DIR}/spv_shaders.h")
    endif()
    if(NOT ADD_GLSL_SHADER_CPP_DESTINATION_SOURCE)
        set(ADD_GLSL_SHADER_CPP_DESTINATION_SOURCE "${COMPILED_GLSL_DIR}/spv_shaders.cc")
    endif()

    cmake_path(GET ADD_GLSL_SHADER_CPP_DESTINATION_HEADER PARENT_PATH header_parent)
    cmake_path(GET ADD_GLSL_SHADER_CPP_DESTINATION_SOURCE PARENT_PATH source_parent)
    if (NOT header_parent MATCHES ${source_parent})
        message(FATAL_ERROR "target_add_glsl_shaders_cpp(): DESTINATION_HEADER and DESTINATION_SOURCE must be within the same directory")
    endif()

    cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${header_parent}" is_binary_subdir)
    if(NOT EXISTS "${header_parent}")
        if(is_binary_subdir OR OVERRIDE_GLSL_SHADER_DESTINATION_SUBDIR_SAFETY)
            file(MAKE_DIRECTORY "${header_parent}")
            file(TOUCH "${ADD_GLSL_SHADER_CPP_DESTINATION_SOURCE}")
        else()
            message(FATAL_ERROR "Refusing to generate GLSL C++ file that is outside of the CMake build directory. Set OVERRIDE_GLSL_SHADER_DESTINATION_SUBDIR_SAFETY=ON to override.")
        endif()
    endif()

    # Create individual glslc compile commands for reach GLSL source specified
    set(temp_cspv "")
    foreach(source ${ADD_GLSL_SHADER_CPP_SOURCES})
        cmake_path(GET source FILENAME filename)
        set(output_file "${header_parent}/${filename}.cspv")
        set(config_args
            "$<$<CONFIG:Debug>:${GLSLC_FLAGS_DEBUG}>"
            "$<$<CONFIG:Release>:${GLSLC_FLAGS_RELEASE}>"
            "$<$<CONFIG:MinSizeRel>:${GLSLC_FLAGS_MINSIZEREL}>"
            "$<$<CONFIG:RelWithDebInfo>:${GLSLC_FLAGS_RELWITHDEBINFO}>")

        add_custom_command(
            OUTPUT ${output_file}
            COMMAND ${Vulkan_GLSLC_EXECUTABLE} ${GLSLC_FLAGS} "${config_args}" "${source}" "-mfmt=num" "-o" "${output_file}"
            MAIN_DEPENDENCY "${source}"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            COMMAND_EXPAND_LISTS)
        list(APPEND temp_cspv "${output_file}")
    endforeach()

    # Generate C++ header file declaring shader global shader bytecode instances.
    # 'spv_shaders.h.in' acts as the template, and expands the 'extern_shader_decls' variable to populate itself.
    set(extern_shader_decls "")
    foreach(id ${ADD_GLSL_SHADER_CPP_IDENTIFIERS})
        string(APPEND extern_shader_decls "extern const SpirVShaderBytecode ${id};\n")
    endforeach()
    configure_file("${PROJECT_SOURCE_DIR}/cmake/spv_shaders.h.in" "${ADD_GLSL_SHADER_CPP_DESTINATION_HEADER}")

    # Add custom command to execute final step. This same CMake file, run as a script rather than a configuration
    # list, reads the SPIR-V bytecode generated by prior calls, and generates a C++ source file containing the 
    # bytecode and global variable defintions. 
    add_custom_command(OUTPUT ${ADD_GLSL_SHADER_CPP_DESTINATION_SOURCE}
        COMMAND "cmake" "-P" "${PROJECT_SOURCE_DIR}/cmake/ShaderCompilation.cmake" "--"
            "DESTINATION_HEADER" "${ADD_GLSL_SHADER_CPP_DESTINATION_HEADER}"
            "DESTINATION_SOURCE" "${ADD_GLSL_SHADER_CPP_DESTINATION_SOURCE}"
            "INPUTS" "${temp_cspv}" "IDS" "${ADD_GLSL_SHADER_CPP_IDENTIFIERS}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        MAIN_DEPENDENCY "${ADD_GLSL_SHADER_CPP_DESTINATION_HEADER}"
        DEPENDS "${temp_cspv}"
        COMMAND_EXPAND_LISTS)

    # Added header/source as dependencies of the target
    target_include_directories(${target} PRIVATE ${header_parent})
    target_sources(${target} PRIVATE ${ADD_GLSL_SHADER_CPP_DESTINATION_SOURCE})
endfunction()
