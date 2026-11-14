add_library(jowe_convert STATIC
  core/common.cpp
  core/convert.cpp
  core/formats.cpp
  core/plugins.cpp
  image/image.cpp
  image/stb_impl.cpp
  image/tiff.cpp
  image/webp.cpp
  image/heif.cpp
  image/os_image.cpp
  audio/audio.cpp
  audio/pcm_formats.cpp
  audio/flac.cpp
  audio/mp3.cpp
  audio/os_audio.cpp
  video/isobmff.cpp
  video/video.cpp
  video/nal.cpp
  video/backend.cpp
  video/backend_mf.cpp
  video/backend_vt.cpp
  video/backend_none.cpp
  video/transcode.cpp
  model/model.cpp
  model/assimp_io.cpp
  model/usd_read.cpp
  model/usd_write.cpp
)
add_library(jowe::convert ALIAS jowe_convert)

target_include_directories(jowe_convert
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

target_include_directories(jowe_convert SYSTEM PRIVATE
  ${tinyusdz_SOURCE_DIR}/src
  ${libwebp_SOURCE_DIR}/src
  ${flac_SOURCE_DIR}/include)

target_link_libraries(jowe_convert
  PRIVATE
    assimp::assimp
    tinyusdz::tinyusdz_static
    webp
    FLAC
    minimp3_headers
    jc_stb_headers)

target_compile_definitions(jowe_convert PRIVATE FLAC__NO_DLL)

if(JOWE_CONVERT_WITH_HEIC_ENCODER)
  target_link_libraries(jowe_convert PRIVATE kvazaar)
  target_compile_definitions(jowe_convert PRIVATE JOWE_CONVERT_HAS_KVAZAAR KVZ_STATIC_LIB)
  target_include_directories(jowe_convert SYSTEM PRIVATE ${kvazaar_SOURCE_DIR}/src)
endif()

if(JOWE_CONVERT_WITH_OS_CODECS AND NOT EMSCRIPTEN)
  target_compile_definitions(jowe_convert PRIVATE JOWE_CONVERT_OS_CODECS)
  if(WIN32)
    target_link_libraries(jowe_convert PRIVATE windowscodecs mfplat mfreadwrite mfuuid ole32)
  elseif(APPLE)
    target_link_libraries(jowe_convert PRIVATE
      "-framework CoreFoundation" "-framework CoreGraphics" "-framework ImageIO" "-framework AudioToolbox"
      "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo")
  endif()
endif()

if(NOT WIN32 AND NOT EMSCRIPTEN)
  target_link_libraries(jowe_convert PRIVATE ${CMAKE_DL_LIBS})
endif()

if(MSVC)
  target_compile_options(jowe_convert PRIVATE /W4 /permissive- /utf-8 /wd4100 /wd4244 /wd4267 /wd4996)
  target_compile_definitions(jowe_convert PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  target_compile_options(jowe_convert PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()
