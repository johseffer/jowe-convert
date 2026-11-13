include(FetchContent)

# Every dependency is a release archive from its official GitHub repo, pinned by tag and SHA-256.
# minimp3 has never published a tag, so it is pinned to a commit.

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

function(jc_declare name url sha256)
  FetchContent_Declare(${name}
    URL ${url}
    URL_HASH SHA256=${sha256}
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL
    SYSTEM
    ${ARGN})
endfunction()

jc_declare(assimp
  https://github.com/assimp/assimp/archive/refs/tags/v6.0.5.tar.gz
  edf3749559c2b7d1f758ffb66fc5bec62186221e623b7f2e8969f17ee46ecb6f)
jc_declare(tinyusdz
  https://github.com/lighttransport/tinyusdz/archive/refs/tags/v0.9.1.tar.gz
  b2ca6fd02458e1db269ef3b74b8ffa0f253b7bf449e3a2b4db67863621eda6d8)
jc_declare(libwebp
  https://github.com/webmproject/libwebp/archive/refs/tags/v1.6.0.tar.gz
  93a852c2b3efafee3723efd4636de855b46f9fe1efddd607e1f42f60fc8f2136)
jc_declare(flac
  https://github.com/xiph/flac/archive/refs/tags/1.5.0.tar.gz
  aea54ed186ad07a34750399cb27fc216a2b62d0ffcd6dc2e3064a3518c3146f8)
jc_declare(minimp3
  https://github.com/lieff/minimp3/archive/ea99364f61c14656440e8d77e9c233ccf3124633.tar.gz
  5628166eb82a9bb581317918a334c317a2c0a30278bb14a20381307976768f34
  SOURCE_SUBDIR headers-only)
if(JOWE_CONVERT_WITH_HEIC_ENCODER)
  jc_declare(kvazaar
    https://github.com/ultravideo/kvazaar/archive/refs/tags/v2.3.2.tar.gz
    ddd0038696631ca5368d8e40efee36d2bbb805854b9b1dda8b12ea9b397ea951)
endif()

# Assimp: only the importers and exporters we expose; FBX is import only.
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_VIEW OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
# Assimp's bundled zlib breaks on recent macOS SDKs (it redefines fdopen); macOS ships zlib.
if(APPLE)
  set(ASSIMP_BUILD_ZLIB OFF CACHE BOOL "" FORCE)
else()
  set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
endif()
set(ASSIMP_BUILD_DRACO OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_USE_CCACHE OFF CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
foreach(fmt OBJ GLTF FBX COLLADA STL PLY 3DS)
  set(ASSIMP_BUILD_${fmt}_IMPORTER ON CACHE BOOL "" FORCE)
endforeach()
foreach(fmt OBJ GLTF STL PLY)
  set(ASSIMP_BUILD_${fmt}_EXPORTER ON CACHE BOOL "" FORCE)
endforeach()

# TinyUSDZ: the reader plus Tydra; no examples, tools or optional importers.
set(TINYUSDZ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_USE_CCACHE OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_WITH_C_API OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_WITH_TYDRA ON CACHE BOOL "" FORCE)
set(TINYUSDZ_WITH_USD_TO_GLTF OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_WITH_USDOBJ OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_WITH_USDFBX OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_WITH_USDVOX OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_WITH_AUDIO OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_WITH_EXR OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_WITH_TIFF OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_WITH_COLORIO OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_WITH_PYTHON OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_WITH_PXR_COMPAT_API OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_NO_WERROR ON CACHE BOOL "" FORCE)

# libwebp: the codec libraries only.
foreach(tool ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO WEBPMUX EXTRAS)
  set(WEBP_BUILD_${tool} OFF CACHE BOOL "" FORCE)
endforeach()
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
if(EMSCRIPTEN)
  set(WEBP_USE_THREAD OFF CACHE BOOL "" FORCE)
  set(WEBP_ENABLE_SIMD OFF CACHE BOOL "" FORCE)
endif()

# libFLAC: the C library only, no Ogg.
set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(WITH_OGG OFF CACHE BOOL "" FORCE)
set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
set(INSTALL_PKGCONFIG_MODULES OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(ENABLE_MULTITHREADING OFF CACHE BOOL "" FORCE)
set(WITH_FORTIFY_SOURCE OFF CACHE BOOL "" FORCE)
set(WITH_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)

# kvazaar: the static library only.
set(BUILD_KVAZAAR_BINARY OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(USE_CRYPTO OFF CACHE BOOL "" FORCE)

set(_jc_deps assimp tinyusdz libwebp flac minimp3)
if(JOWE_CONVERT_WITH_HEIC_ENCODER)
  list(APPEND _jc_deps kvazaar)
endif()

set(_jc_saved_build_type "${CMAKE_BUILD_TYPE}")
FetchContent_MakeAvailable(${_jc_deps})
set(CMAKE_BUILD_TYPE "${_jc_saved_build_type}" CACHE STRING "Build type" FORCE)

# TinyUSDZ 0.9.1 relies on <cstdint> arriving transitively, which newer GCC no longer does.
if(NOT MSVC)
  target_compile_options(tinyusdz_static PRIVATE -include cstdint)
endif()

if(NOT TARGET minimp3_headers)
  add_library(minimp3_headers INTERFACE)
  target_include_directories(minimp3_headers SYSTEM INTERFACE ${minimp3_SOURCE_DIR})
endif()

# stb_image, stb_image_write and stb_image_resize2 (MIT / public domain) ship inside TinyUSDZ;
# src/image/stb_impl.cpp compiles our own static copy of them.
add_library(jc_stb_headers INTERFACE)
target_include_directories(jc_stb_headers SYSTEM INTERFACE ${tinyusdz_SOURCE_DIR}/src/external)

if(JOWE_CONVERT_WITH_HEIC_ENCODER AND TARGET kvazaar)
  target_compile_definitions(kvazaar INTERFACE KVZ_STATIC_LIB)
endif()
