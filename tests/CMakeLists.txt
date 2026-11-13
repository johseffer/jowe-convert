set(_jc_test_names
  image_roundtrip_formats
  image_tiff_lossless
  image_resize_and_quality
  image_heic_encode
  image_errors
  audio_pcm_containers
  audio_resample_and_channels
  audio_mp3_decode
  audio_encoded_targets
  video_remux_mp4_mov
  video_errors
  model_obj_usdz_glb_roundtrip
  model_all_writers
  model_units_and_axis
  model_no_textures_and_errors
  plugin_load_and_encode
  plugin_abi_rejected)

add_executable(jc_tests
  test_main.cpp
  test_image.cpp
  test_audio.cpp
  test_video.cpp
  test_model.cpp
  test_plugin.cpp)
target_include_directories(jc_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(jc_tests PRIVATE jowe::convert)
target_compile_definitions(jc_tests PRIVATE JC_MINIMP3_VECTORS="${minimp3_SOURCE_DIR}/vectors")
if(MSVC)
  target_compile_options(jc_tests PRIVATE /utf-8 /wd4996)
  target_compile_definitions(jc_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

if(EMSCRIPTEN)
  target_link_options(jc_tests PRIVATE -sNODERAWFS=1 -sEXIT_RUNTIME=1)
else()
  add_library(jc_test_encoder MODULE plugin/test_encoder.c)
  target_include_directories(jc_test_encoder PRIVATE ${PROJECT_SOURCE_DIR}/include)
  set_target_properties(jc_test_encoder PROPERTIES C_VISIBILITY_PRESET hidden PREFIX "")
  add_dependencies(jc_tests jc_test_encoder)
  target_compile_definitions(jc_tests PRIVATE JC_TEST_PLUGIN="$<TARGET_FILE:jc_test_encoder>")
endif()

foreach(name IN LISTS _jc_test_names)
  add_test(NAME ${name} COMMAND jc_tests ${name})
endforeach()

if(TARGET jvconvert)
  add_test(NAME mcp_stdio_session
    COMMAND ${CMAKE_COMMAND}
      -DJVCONVERT=$<TARGET_FILE:jvconvert>
      "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/mcp_session
      -P ${CMAKE_CURRENT_SOURCE_DIR}/mcp_session.cmake)
  add_test(NAME cli_smoke
    COMMAND ${CMAKE_COMMAND}
      -DJVCONVERT=$<TARGET_FILE:jvconvert>
      "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli_smoke
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_smoke.cmake)
endif()
