cmake_minimum_required(VERSION 3.21)

project(BlinkDash VERSION 1.0.0 LANGUAGES CXX)

option(ENABLE_SKYRIM_SE "Build against the Skyrim SE runtime" ON)
option(ENABLE_SKYRIM_AE "Build against the Skyrim AE runtime" ON)
option(ENABLE_SKYRIM_VR "Build against the Skyrim VR runtime" OFF)

# ---------------------------------------------------------------------------
# Version header
# ---------------------------------------------------------------------------
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in"
	"${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h"
	@ONLY
)

# ---------------------------------------------------------------------------
# CommonLibSSE-NG (alandtse fork, ng branch) - single binary SE+AE+VR
# ---------------------------------------------------------------------------
set(COMMONLIBSSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibSSE")
if(NOT EXISTS "${COMMONLIBSSE_DIR}/CMakeLists.txt")
	find_package(Git REQUIRED)
	message(STATUS "extern/CommonLibSSE not found - cloning alandtse/CommonLibSSE-NG (ng branch)...")
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" clone --branch ng --depth 1 --recurse-submodules --shallow-submodules https://github.com/alandtse/CommonLibSSE-NG "${COMMONLIBSSE_DIR}"
		RESULT_VARIABLE _clsse_clone_result
	)
	if(NOT _clsse_clone_result EQUAL 0)
		message(FATAL_ERROR "Failed to clone CommonLibSSE-NG automatically.")
	endif()
endif()

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${COMMONLIBSSE_DIR}" CommonLibSSE)

# The helper is only auto-included in CommonLibSSE's prebuilt branch; include it for source builds too.
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
