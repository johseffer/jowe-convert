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

/*
 * Video codec backends.
 *
 * Video transcoding uses the OS codecs. Windows (Media Foundation) and Apple (VideoToolbox) backends are built in;
 * other hosts (Android MediaCodec, WebCodecs on the web) register a table like this one with
 * jowe::convert::registerVideoBackend(). The library demuxes, scales, drops frames for a frame-rate cap and muxes;
 * the backend only decodes compressed samples to frames and encodes frames to compressed samples.
 *
 * Rules:
 *  - Codec names are ISO-BMFF sample entry types: "avc1" (H.264) or "hvc1" (HEVC).
 *  - Compressed samples, in and out, are MP4-style: NAL units each prefixed by a 4-byte big-endian length.
 *  - Decoder configuration is the avcC / hvcC record (the box payload, without the box header).
 *  - Decoders may emit frames in any order; the library reorders them by pts. Encoders must not reorder
 *    (no B-frames), so a packet's dts equals its pts.
 *  - Frames are NV12 (8-bit, video range). Width and height are even.
 *  - Calls are synchronous: every function returns only after its callbacks for the frames / packets it produced
 *    have run, or holds them for a later push / flush.
 */

#define JC_VIDEO_BACKEND_ABI_VERSION 1u

enum { JC_PIX_NV12 = 1 };

typedef struct jc_video_frame {
  uint32_t struct_size;
  uint32_t format; /* JC_PIX_NV12 */
  uint32_t width;
  uint32_t height;
  const uint8_t* plane[2]; /* Y, interleaved UV */
  uint32_t stride[2];
  int64_t pts_us;
} jc_video_frame;

typedef int32_t (*jc_video_frame_fn)(void* user, const jc_video_frame* frame);
typedef int32_t (*jc_video_packet_fn)(void* user, const uint8_t* data, size_t size, int64_t pts_us, int64_t dts_us,
                                      int32_t keyframe);

typedef struct jc_video_encoder_config {
  uint32_t struct_size;
  const char* codec; /* "avc1" or "hvc1" */
  uint32_t width;
  uint32_t height;
  uint32_t fps_num;
  uint32_t fps_den;
  uint32_t bitrate_kbps;
  uint32_t keyframe_interval; /* frames */
  /* Present when struct_size covers it. The library leaves it 0 (no B-frames); it is only raised to make test
     fixtures with reordered frames. */
  uint32_t max_b_frames;
} jc_video_encoder_config;

typedef struct jc_video_decoder jc_video_decoder;
typedef struct jc_video_encoder jc_video_encoder;

typedef struct jc_video_backend {
  uint32_t abi_version;
  uint32_t struct_size;
  const char* name;

  /* 1 when this codec can be decoded / encoded here, else 0 with the reason. */
  int32_t (*can_decode)(const char* codec, char* reason, size_t reason_size);
  int32_t (*can_encode)(const char* codec, char* reason, size_t reason_size);

  jc_video_decoder* (*decoder_create)(const char* codec, const uint8_t* config, size_t config_size, uint32_t width,
                                      uint32_t height, char* err, size_t err_size);
  int32_t (*decoder_push)(jc_video_decoder* dec, const uint8_t* sample, size_t size, int64_t pts_us,
                          int32_t keyframe, jc_video_frame_fn on_frame, void* user, char* err, size_t err_size);
  int32_t (*decoder_flush)(jc_video_decoder* dec, jc_video_frame_fn on_frame, void* user, char* err,
                           size_t err_size);
  void (*decoder_destroy)(jc_video_decoder* dec);

  jc_video_encoder* (*encoder_create)(const jc_video_encoder_config* config, char* err, size_t err_size);
  int32_t (*encoder_push)(jc_video_encoder* enc, const jc_video_frame* frame, jc_video_packet_fn on_packet,
                          void* user, char* err, size_t err_size);
  int32_t (*encoder_flush)(jc_video_encoder* enc, jc_video_packet_fn on_packet, void* user, char* err,
                           size_t err_size);
  /* The avcC / hvcC payload, available once the first packet was produced. Returns JC_OK when written. */
  int32_t (*encoder_config)(jc_video_encoder* enc, uint8_t* out, size_t capacity, size_t* size);
  void (*encoder_destroy)(jc_video_encoder* enc);
} jc_video_backend;

#ifdef __cplusplus
}
#endif

#endif
