
cmake_minimum_required(VERSION 3.20)

#To configure the layer name, description and output name, change these variables
project(UVOSuit-Vignette-Layer)
set(library_name      "uvosuit_vignette")
set(layer_name        "XR_APILAYER_UVOSUIT_vignette") # This name *must* be of the form XR_APILAYER_<VENDOR_TAG>_the_name_of_the_layer to be valid according to the OpenXR 1.0 spec.
set(layer_version     "1")
set(layer_description "Ultimate VR Optics Suite - Vignette Layer")

#To add extension declaration to the layer manifest file, fill these two string lists : "extensions" and "extension_versions"
#These two needs to match in lenght
#list(APPEND extensions "XR_TEST_test_me")
#list(APPEND extension_versions "42")

#this need to be done in the exact same pattern for every extensions this OpenXR layer implements.
#the size of the two list *have* to match
#list(APPEND extensions "XR_EXT_b")
#list(APPEND extension_versions "2")
#list(APPEND extensions "XR_EXT_c")
#list(APPEND extension_versions "3")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/)
include(extension_list_helper)
build_instance_extension_list("${extensions}" "${extension_versions}" instance_extension_list)
build_instance_extension_header("${extensions}" "${extension_versions}" instance_extension_header)

set(layer_directory "${CMAKE_BINARY_DIR}/${layer_name}/")
file(MAKE_DIRECTORY ${layer_directory})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${layer_directory})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${layer_directory})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${layer_directory})
set(current_folder_path_prefix "./") # This works on windows too now with up-to-date OpenXR loaders
set(CMAKE_CXX_STANDARD 17)

#some platform specific twidling of filenames and path conventions
if(WIN32)
	set(library_filename "${library_name}.dll")
	if(msvc)
	add_compile_definitions(/w26812)
	endif()
else()
	set(library_filename "${library_name}.so")
endif(WIN32)

configure_file(
	${CMAKE_SOURCE_DIR}/src/layer_config.hpp.template
	${CMAKE_SOURCE_DIR}/src/layer_config.hpp
)

configure_file(
	${CMAKE_SOURCE_DIR}/src/manifest.json.template
	${CMAKE_SOURCE_DIR}/src/manifest.json
)

#TODO make this a bit nicer
file(GLOB layer_src CONFIGURE_DEPENDS src/*)


set_source_files_properties(src/VignetteVS.hlsl PROPERTIES
    VS_SHADER_TYPE "Vertex"
    VS_SHADER_MODEL "5.0"
    VS_SHADER_OUTPUT_HEADER_FILE "%(RelativeDir)%(Filename).h"
    VS_SHADER_VARIABLE_NAME "g_VignetteVS"
)

set_source_files_properties(src/VignettePS.hlsl PROPERTIES
    VS_SHADER_TYPE "Pixel"
    VS_SHADER_MODEL "5.0"
    VS_SHADER_OUTPUT_HEADER_FILE "%(RelativeDir)%(Filename).h"
    VS_SHADER_VARIABLE_NAME "g_VignettePS"
)

add_library(${library_name} SHARED ${layer_src} ${CMAKE_SOURCE_DIR}/README.md)
target_include_directories(${library_name} PRIVATE
	${CMAKE_SOURCE_DIR}/OpenXR-SDK/src/loader
	${CMAKE_SOURCE_DIR}/OpenXR-SDK/src/common
	${CMAKE_SOURCE_DIR}/OpenXR-SDK/include)

add_custom_target(install_manifest
	COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/src/manifest.json ${layer_directory})
add_dependencies(${library_name} install_manifest)
