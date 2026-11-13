/*
 * Stable C ABI for jowe-convert encoder plugins.
 *
 * An encoder that cannot live in the Apache-2.0 core (for example an LGPL MP3 encoder) is built as its own
 * shared library and loaded at run time. The library exports one function, jc_encoder_plugins(), which
 * returns a table of encoders. The host only calls the functions below; it never sees the encoder's types.
 *
 * Rules:
 *  - Strings returned by the plugin stay valid while the library is loaded.
 *  - All functions return JC_OK (0) on success; on failure they write a message into err (NUL terminated).
 *  - Audio arrives as interleaved 32-bit float in [-1, 1]. Encoded bytes go out through the write callback.
 *  - A plugin built for ABI version N refuses a host with a different major ABI version.
 */
#ifndef JOWE_CONVERT_PLUGIN_H
#define JOWE_CONVERT_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JC_PLUGIN_ABI_VERSION 1u

#if defined(_WIN32)
#define JC_PLUGIN_EXPORT __declspec(dllexport)
#else
#define JC_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

enum {
  JC_OK = 0,
  JC_ERR_INVALID = 1,
  JC_ERR_UNSUPPORTED = 2,
  JC_ERR_IO = 3,
  JC_ERR_FAILED = 4
};

enum { JC_KIND_AUDIO = 1 };

/* Called by the encoder with each piece of output, in order. Returns JC_OK or an error. */
typedef int32_t (*jc_write_fn)(void* user, const uint8_t* data, size_t size);

typedef struct jc_audio_encoder_config {
  uint32_t struct_size;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t bitrate_kbps; /* 0 = encoder default */
  int32_t quality;       /* 0..100, -1 = encoder default */
} jc_audio_encoder_config;

typedef struct jc_encoder jc_encoder;

typedef struct jc_encoder_plugin {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t kind;
  const char* format_id;  /* lower-case id, e.g. "mp3" */
  const char* extensions; /* comma separated, e.g. "mp3" */
  const char* name;
  const char* version;
  const char* license;    /* SPDX id, e.g. "LGPL-2.0-or-later" */

  /* Returns 1 when the encoder can run here, 0 otherwise (with the reason in reason). */
  int32_t (*probe)(char* reason, size_t reason_size);

  /* Accepts or adjusts a configuration (e.g. clamps the sample rate). Returns JC_OK if usable. */
  int32_t (*negotiate)(jc_audio_encoder_config* config, char* err, size_t err_size);

  jc_encoder* (*create)(const jc_audio_encoder_config* config, jc_write_fn write, void* user,
                        char* err, size_t err_size);
  int32_t (*write_f32)(jc_encoder* enc, const float* interleaved, size_t frames, char* err, size_t err_size);
  int32_t (*finish)(jc_encoder* enc, char* err, size_t err_size);
  void (*destroy)(jc_encoder* enc);
} jc_encoder_plugin;

/* The one symbol a plugin library exports. */
typedef const jc_encoder_plugin* const* (*jc_encoder_plugins_fn)(uint32_t host_abi_version, size_t* count);
#define JC_ENCODER_PLUGINS_SYMBOL "jc_encoder_plugins"

#ifdef __cplusplus
}
#endif

#endif
