cmake_minimum_required(VERSION 3.21)

# The release version, baked into the binary (Version.h) and the SKSE plugin record. It is a
# cache variable so tools/Build-Release.ps1 can stamp the DLL with the version it names the
# archive after (-DBLINKDASH_VERSION=1.0.1) without anyone editing this file per release.
set(BLINKDASH_VERSION "1.2.1" CACHE STRING "Version baked into the plugin binary")

project(BlinkDash VERSION ${BLINKDASH_VERSION} LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Version header
# ---------------------------------------------------------------------------
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in"
	"${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h"
	@ONLY
)

# ---------------------------------------------------------------------------
# CommonLibSSE-NG (alandtse fork) - single binary SE+AE+VR, NOT compiled here.
#
# The library is consumed the way the project documents it: a source checkout pinned to a
# clean release tag plus add_subdirectory, where the library's own cmake/Prebuilt.cmake
# replaces "compile ~530 translation units" with the prebuilt static library that release
# publishes (SHA256-verified, cached under the build tree as .prebuilt/<tag>).
# COMMONLIB_PREBUILT is set in the preset; CI gets it for free because the resolver also
# triggers on GITHUB_ACTIONS. COMMONLIB_PREBUILT_DIR=<extracted bundle> overrides the
# download entirely (offline machines).
#
# Three conditions come baked into that bundle and must not be broken:
#  * every runtime stays on (SE + AE + VR). The runtime set changes public header layout, so
#    VR off is not "smaller", it is ABI-incompatible - the resolver refuses and rebuilds from
#    source. This is why the options are not overridden here any more (defaults are all on).
#  * Release + the dynamic CRT (/MD, i.e. the x64-windows-static-md triplet): the bundle is /MD.
#  * an MSVC toolset sharing major.minor with the one that produced the bundle.
# Every mismatch falls back to a source build automatically - it just costs the ~16 minutes
# this exists to avoid.
# ---------------------------------------------------------------------------
set(COMMONLIBSSE_TAG v13.0.0)
set(COMMONLIBSSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibSSE")
if(NOT EXISTS "${COMMONLIBSSE_DIR}/CMakeLists.txt")
	find_package(Git REQUIRED)
	message(STATUS "extern/CommonLibSSE not found - cloning alandtse/CommonLibSSE-NG at ${COMMONLIBSSE_TAG}...")
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" clone --branch ${COMMONLIBSSE_TAG} --depth 1 --recurse-submodules --shallow-submodules https://github.com/alandtse/CommonLibSSE-NG "${COMMONLIBSSE_DIR}"
		RESULT_VARIABLE _clsse_clone_result
	)
	if(NOT _clsse_clone_result EQUAL 0)
		message(FATAL_ERROR "Failed to clone CommonLibSSE-NG automatically.")
	endif()
endif()

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${COMMONLIBSSE_DIR}" CommonLibSSE)

# The prebuilt branch includes this helper itself; a source build does not, so include it.
include("${COMMONLIBSSE_DIR}/cmake/CommonLibSSE.cmake")

# ---------------------------------------------------------------------------
# vcpkg dependencies
# ---------------------------------------------------------------------------
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Plugin sources
# ---------------------------------------------------------------------------
set(SOURCES
	src/PCH.h
	src/Plugin.cpp
	src/Version.h.in
	src/Log.h
	src/Blink.h
	src/Blink.cpp
	src/Config.h
	src/Config.cpp
	src/Effects.h
	src/Effects.cpp
	src/Debug.h
	src/Debug.cpp
	src/Input.h
	src/Perks.h
	src/Perks.cpp
	src/UI.h
	src/UI.cpp
	extern/SKSEMenuFramework.h
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${SOURCES})

add_commonlibsse_plugin(${PROJECT_NAME}
	NAME "BlinkDash"
	AUTHOR "BlinkDash"
	VERSION "${PROJECT_VERSION}"
	USE_ADDRESS_LIBRARY
	SOURCES ${SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
	src
	extern
	"${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
if(MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
	CommonLibSSE::CommonLibSSE
	spdlog::spdlog
	nlohmann_json::nlohmann_json
)

set_target_properties(${PROJECT_NAME} PROPERTIES
	PREFIX ""
	OUTPUT_NAME "${PROJECT_NAME}"
)
target_link_options(${PROJECT_NAME} PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/BlinkDash.map")

# ---------------------------------------------------------------------------
# Copy DLL into the MO2 mod folder on build.
# MOD_OUTPUT_FOLDER is not set by the committed preset: set it locally (for example
# in a git-ignored CMakeUserPresets.json) to a mod folder to enable auto-deploy.
# ---------------------------------------------------------------------------
if(MOD_OUTPUT_FOLDER)
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "${MOD_OUTPUT_FOLDER}/SKSE/Plugins"
		COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${PROJECT_NAME}>" "${MOD_OUTPUT_FOLDER}/SKSE/Plugins/"
		# BlinkDash.json is intentionally NOT copied: it holds the user's saved
		# settings and the plugin writes it with the defaults on first run.
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/dist/BlinkDash.ShaderNames.json" "${MOD_OUTPUT_FOLDER}/SKSE/Plugins/"
		VERBATIM
	)
endif()
