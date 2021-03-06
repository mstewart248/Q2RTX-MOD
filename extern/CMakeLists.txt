
#
# ZLIB
#

if (NOT CONFIG_LINUX_STEAM_RUNTIME_SUPPORT)
    add_subdirectory(zlib)
    target_include_directories(zlibstatic PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/zlib> $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/zlib>)

    set_target_properties(zlib PROPERTIES FOLDER extern)
    set_target_properties(zlibstatic PROPERTIES FOLDER extern)
    set_target_properties(minigzip PROPERTIES FOLDER extern)
    set_target_properties(example PROPERTIES FOLDER extern)
endif()

#
# glslangValidator
#

if(CONFIG_BUILD_GLSLANG)
    option(BUILD_EXTERNAL OFF)
    option(ENABLE_SPVREMAPPER OFF)
    option(ENABLE_HLSL OFF)
    option(ENABLE_OPT OFF)
    option(ENABLE_PCH OFF)
    option(ENABLE_CTEST OFF)

    add_subdirectory(glslang)

    set_target_properties(glslangValidator PROPERTIES FOLDER glslang)
    set_target_properties(glslang-build-info PROPERTIES FOLDER glslang)
endif()

#
# SDL 2
#

option(SDL_SHARED"" OFF)
option(SDL_STATIC"" ON)
option(SDL_SHARED_ENABLED_BY_DEFAULT OFF)

if (CONFIG_LINUX_STEAM_RUNTIME_SUPPORT)
	option(INPUT_TSLIB OFF)
endif()

add_subdirectory(SDL2)

set_target_properties(SDL2main PROPERTIES FOLDER extern)
set_target_properties(SDL2-static PROPERTIES FOLDER extern)
set_target_properties(uninstall PROPERTIES FOLDER extern)

#
# CURL
#

if(CONFIG_USE_CURL)

    option(BUILD_CURL_EXE "" OFF)
    option(BUILD_SHARED_LIBS "" OFF)
    option(CURL_STATICLIB "" ON)
    option(BUILD_TESTING "" OFF)
    option(ENABLE_MANUAL "" OFF)
    option(ENABLE_DOCS "" OFF)
    option(HTTP_ONLY "" ON)
    option(CURL_ZLIB "" OFF)
    option(CMAKE_USE_OPENSSL "" OFF)
    set(CURL_CA_PATH "none" CACHE PATH "")

    set(CMAKE_DEBUG_POSTFIX "" CACHE STRING "Override CMAKE_DEBUG_POSTFIX, which curl sets to '-d'")
    
    set(ZLIB_DIR "{CMAKE_CURRENT_SOURCE_DIR}/zlib")

    add_subdirectory(curl)

    set_target_properties(libcurl PROPERTIES FOLDER extern)
endif()


add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/stb)

add_library(tinyobjloader INTERFACE)
target_include_directories(tinyobjloader INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tinyobjloader-c)
