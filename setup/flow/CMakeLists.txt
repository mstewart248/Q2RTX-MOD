# Builds nvflow and nvflowext - the NVIDIA Flow 2 runtime that
# src/refresh/vkpt/flow.c loads - from the Flow source in FLOW_ROOT, without
# Flow's own premake/packman bootstrap. setup/build_flow.py drives it; see
# there for the pinned source and the patches applied first.
#
# The project mirrors flow/premake5.lua: the shader compiler (HLSL -> SPIR-V
# through Slang) and the shader tool that runs it over a .nfproj are built
# first, the tool generates each library's shader headers, then the libraries
# build against them. The editor and the *_rtx variants are left out.

cmake_minimum_required(VERSION 3.16)
project(nvflow CXX)

if(NOT FLOW_ROOT)
	message(FATAL_ERROR "set FLOW_ROOT to the flow/ directory of the PhysX repository")
endif()

if(WIN32)
	set(FLOW_PLATFORM windows-x86_64)
else()
	set(FLOW_PLATFORM linux-x86_64)
endif()

# nvflowshadertool runs the compiler from a path it has compiled in,
# "../../../_build/<platform>/release/nvflowshadercompiler", relative to the
# directory it is started in, and the .nfproj files name their include
# directories relative to that same place. So the compiler goes exactly
# there, and the tool runs from a directory three levels under FLOW_ROOT.
set(FLOW_BIN ${FLOW_ROOT}/_build/${FLOW_PLATFORM}/release)
set(FLOW_GENERATED_REL ../../../_generated/${FLOW_PLATFORM}/release)
set(FLOW_GENERATED ${FLOW_ROOT}/_generated/${FLOW_PLATFORM}/release)
set(FLOW_TOOL_CWD ${FLOW_ROOT}/_compiler/cmake)
file(MAKE_DIRECTORY ${FLOW_BIN} ${FLOW_GENERATED})

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
	# premake: exceptionhandling Off, rtti Off, floatingpoint Fast, symbols On.
	string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
	string(REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
	add_compile_options(/GR- /fp:fast /Zi /wd4550)
	add_compile_definitions(_HAS_EXCEPTIONS=0 _CRT_SECURE_NO_WARNINGS)
	add_link_options(/DEBUG /OPT:REF /OPT:ICF)
else()
	add_compile_options(-fno-exceptions -fno-rtti -ffast-math -fvisibility=hidden -msse4)
	add_link_options(-Wl,-rpath,$ORIGIN)
endif()
add_compile_definitions(NDEBUG)

set(FLOW_COMMON_INCLUDES
	${FLOW_ROOT}/shared
	${FLOW_ROOT}/include/nvflow
	${FLOW_ROOT}/include/nvflow/shaders
	${FLOW_GENERATED})

file(GLOB FLOW_SHARED_SOURCES ${FLOW_ROOT}/shared/*.cpp)

function(flow_tool name)
	file(GLOB sources ${FLOW_ROOT}/source/${name}/*.cpp)
	add_executable(${name} ${FLOW_SHARED_SOURCES} ${sources})
	target_include_directories(${name} PRIVATE ${FLOW_COMMON_INCLUDES})
	set_target_properties(${name} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY ${FLOW_BIN}
		RUNTIME_OUTPUT_DIRECTORY_RELEASE ${FLOW_BIN})
	if(NOT WIN32)
		target_link_libraries(${name} PRIVATE dl pthread)
	endif()
endfunction()

# --- shader compiler and tool

flow_tool(nvflowshadercompiler)
target_include_directories(nvflowshadercompiler PRIVATE ${FLOW_ROOT}/external/slang/include)
if(WIN32)
	target_link_libraries(nvflowshadercompiler PRIVATE ${FLOW_ROOT}/external/slang/lib/slang.lib)
	set(SLANG_RUNTIME
		${FLOW_ROOT}/external/slang/bin/slang.dll
		${FLOW_ROOT}/external/slang/bin/slang-glslang.dll)
else()
	target_link_libraries(nvflowshadercompiler PRIVATE ${FLOW_ROOT}/external/slang/lib/libslang.so)
	set(SLANG_RUNTIME
		${FLOW_ROOT}/external/slang/lib/libslang.so
		${FLOW_ROOT}/external/slang/lib/libslang-glslang.so)
endif()
add_custom_command(TARGET nvflowshadercompiler POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SLANG_RUNTIME} ${FLOW_BIN})

flow_tool(nvflowshadertool)
add_dependencies(nvflowshadertool nvflowshadercompiler)

# Generates <FLOW_GENERATED>/<lib>/... for one library. The tool rewrites only
# what changed, so it runs on every build.
function(flow_shaders target nfproj)
	file(MAKE_DIRECTORY ${FLOW_TOOL_CWD}/${target})
	add_custom_target(${target}_shaders
		COMMAND $<TARGET_FILE:nvflowshadertool> ${FLOW_GENERATED_REL} ../../../${nfproj}
		WORKING_DIRECTORY ${FLOW_TOOL_CWD}/${target}
		COMMENT "Flow shaders for ${target}"
		VERBATIM)
	add_dependencies(${target}_shaders nvflowshadertool nvflowshadercompiler)
	add_dependencies(${target} ${target}_shaders)
endfunction()

# --- libraries

file(GLOB NVFLOW_SOURCES ${FLOW_ROOT}/source/nvflow/*.cpp)
add_library(nvflow SHARED ${FLOW_SHARED_SOURCES} ${NVFLOW_SOURCES})
target_include_directories(nvflow PRIVATE ${FLOW_COMMON_INCLUDES} ${FLOW_GENERATED}/nvflow)
flow_shaders(nvflow source/nvflow/NvFlow.nfproj)

file(GLOB NVFLOWEXT_SOURCES
	${FLOW_ROOT}/source/nvflowext/*.cpp
	${FLOW_ROOT}/source/nvflowext/vulkan/*.cpp
	${FLOW_ROOT}/source/nvflowext/cpu/*.cpp
	${FLOW_ROOT}/source/nvflowext/opt/*.cpp)
add_library(nvflowext SHARED ${FLOW_SHARED_SOURCES} ${NVFLOWEXT_SOURCES})
target_include_directories(nvflowext PRIVATE ${FLOW_COMMON_INCLUDES}
	${FLOW_GENERATED}/nvflowext
	${FLOW_ROOT}/include/nvflowext
	${FLOW_ROOT}/include/nvflowext/shaders
	${FLOW_ROOT}/external/vulkan)
flow_shaders(nvflowext source/nvflowext/NvFlowExt.nfproj)

foreach(lib nvflow nvflowext)
	set_target_properties(${lib} PROPERTIES PREFIX ""
		RUNTIME_OUTPUT_DIRECTORY ${FLOW_BIN}
		RUNTIME_OUTPUT_DIRECTORY_RELEASE ${FLOW_BIN}
		LIBRARY_OUTPUT_DIRECTORY ${FLOW_BIN})
	if(NOT WIN32)
		set_target_properties(${lib} PROPERTIES PREFIX "lib")
		target_link_libraries(${lib} PRIVATE dl pthread)
	endif()
endforeach()
