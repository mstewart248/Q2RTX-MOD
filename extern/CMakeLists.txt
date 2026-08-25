
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

# On a 32-bit target, only the server is built, and it doesn't need SDL
if (IS_64_BIT)
    option(SDL_SHARED "" OFF)
    option(SDL_STATIC "" ON)
    option(SDL_SHARED_ENABLED_BY_DEFAULT OFF)
    option(SDL_TEST OFF)

    if (CONFIG_LINUX_STEAM_RUNTIME_SUPPORT)
    	option(INPUT_TSLIB OFF)
    endif()

    add_subdirectory(SDL2)

    set_target_properties(SDL2main PROPERTIES FOLDER extern)
    set_target_properties(SDL2-static PROPERTIES FOLDER extern)
    set_target_properties(uninstall PROPERTIES FOLDER extern)
    set_target_properties(sdl_headers_copy PROPERTIES FOLDER extern)
endif()

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

#
# Ogg / Vorbis / Theora - the rerelease cutscenes (1080p30 Theora + Vorbis .ogv)
#
# The targets are declared by hand instead of add_subdirectory() for three
# reasons: libtheora ships no CMakeLists at all (autotools + SCons only),
# libvorbis does find_package(Ogg REQUIRED) which cannot resolve in a
# subdirectory build, and we only want the decoders - no encoders, no installs,
# no exports. The source lists come from each project's lib/Makefile.am.
#
if (IS_64_BIT)
    # libogg. Only generated header is ogg/config_types.h; every platform we
    # build for has stdint.h, so the sized types come from there.
    set(INCLUDE_INTTYPES_H 0)
    set(INCLUDE_STDINT_H 1)
    set(INCLUDE_SYS_TYPES_H 0)
    set(SIZE16 "int16_t")
    set(USIZE16 "uint16_t")
    set(SIZE32 "int32_t")
    set(USIZE32 "uint32_t")
    set(SIZE64 "int64_t")
    set(USIZE64 "uint64_t")
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/ogg/include/ogg/config_types.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/ogg_include/ogg/config_types.h)

    add_library(ogg STATIC
        ogg/src/bitwise.c
        ogg/src/framing.c)
    target_include_directories(ogg PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/ogg/include
        ${CMAKE_CURRENT_BINARY_DIR}/ogg_include)
    set_target_properties(ogg PROPERTIES FOLDER extern)

    # libvorbis. Full codec minus vorbisfile/vorbisenc: the .ogv demux is ours,
    # so the stdio and encoder wrappers are dead weight.
    add_library(vorbis STATIC
        vorbis/lib/mdct.c       vorbis/lib/smallft.c    vorbis/lib/block.c
        vorbis/lib/envelope.c   vorbis/lib/window.c     vorbis/lib/lsp.c
        vorbis/lib/lpc.c        vorbis/lib/analysis.c   vorbis/lib/synthesis.c
        vorbis/lib/psy.c        vorbis/lib/info.c       vorbis/lib/floor1.c
        vorbis/lib/floor0.c     vorbis/lib/res0.c       vorbis/lib/mapping0.c
        vorbis/lib/registry.c   vorbis/lib/codebook.c   vorbis/lib/sharedbook.c
        vorbis/lib/lookup.c     vorbis/lib/bitrate.c)
    target_include_directories(vorbis
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/vorbis/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vorbis/lib)
    target_link_libraries(vorbis PUBLIC ogg)
    set_target_properties(vorbis PROPERTIES FOLDER extern)

    # libtheoradec. decoder_sources from lib/Makefile.am, pure C: the x86 SIMD
    # is GCC inline asm and the x86_vc/ variant is 32-bit __asm, neither of
    # which MSVC accepts on x64, so OC_X86_ASM stays undefined. HAVE_CONFIG_H
    # is likewise left undefined - it is the only thing that pulls config.h.
    add_library(theoradec STATIC
        theora/lib/apiwrapper.c     theora/lib/bitpack.c
        theora/lib/decapiwrapper.c  theora/lib/decinfo.c
        theora/lib/decode.c         theora/lib/dequant.c
        theora/lib/fragment.c       theora/lib/huffdec.c
        theora/lib/idct.c           theora/lib/info.c
        theora/lib/internal.c       theora/lib/quant.c
        theora/lib/state.c)
    target_include_directories(theoradec
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/theora/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/theora/lib)
    target_link_libraries(theoradec PUBLIC ogg)
    set_target_properties(theoradec PROPERTIES FOLDER extern)

    if (MSVC)
        # these are third party sources; do not fail our build on their warnings
        target_compile_options(ogg       PRIVATE /wd4244 /wd4267)
        target_compile_options(vorbis    PRIVATE /wd4244 /wd4267 /wd4305 /wd4996)
        target_compile_options(theoradec PRIVATE /wd4244 /wd4267 /wd4305)
    endif()
endif()

#
# openal-soft (64-bit only, not needed for the 32-bit server)
#

if (IS_64_BIT)
    set(LIBTYPE STATIC)
    option(FORCE_STATIC_VCRT "" ON)
    option(ALSOFT_UTILS "" OFF)
    option(ALSOFT_EXAMPLES "" OFF)
    option(ALSOFT_UPDATE_BUILD_VERSION "" OFF)
    option(ALSOFT_INSTALL "" OFF)
    option(ALSOFT_INSTALL_CONFIG "" OFF)
    option(ALSOFT_INSTALL_HRTF_DATA "" OFF)
    option(ALSOFT_INSTALL_AMBDEC_PRESETS "" OFF)
    option(ALSOFT_INSTALL_EXAMPLES "" OFF)
    option(ALSOFT_INSTALL_UTILS "" OFF)
    add_subdirectory(openal-soft)
    set_target_properties(OpenAL PROPERTIES FOLDER extern/openal-soft)
    set_target_properties(common PROPERTIES FOLDER extern/openal-soft)
    set_target_properties(ex-common PROPERTIES FOLDER extern/openal-soft)
endif()