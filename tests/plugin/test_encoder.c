/* A minimal encoder plugin used by the tests: "JCTEST01", rate, channels, then raw float samples. */
#include <jowe/convert_plugin.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct jc_encoder {
  jc_write_fn write;
  void* user;
  uint32_t channels;
};

static int32_t probe(char* reason, size_t n) {
  (void)reason;
  (void)n;
  return 1;
}

static int32_t negotiate(jc_audio_encoder_config* c, char* err, size_t n) {
  (void)err;
  (void)n;
  if (c->sample_rate > 48000) c->sample_rate = 48000;
  if (c->channels > 2) c->channels = 2;
  return JC_OK;
}

static jc_encoder* create(const jc_audio_encoder_config* c, jc_write_fn write, void* user, char* err, size_t n) {
  if (!c || c->struct_size < sizeof(jc_audio_encoder_config) || !c->channels) {
    snprintf(err, n, "bad config");
    return NULL;
  }
  jc_encoder* e = (jc_encoder*)calloc(1, sizeof(jc_encoder));
  e->write = write;
  e->user = user;
  e->channels = c->channels;
  uint8_t head[16];
  memcpy(head, "JCTEST01", 8);
  memcpy(head + 8, &c->sample_rate, 4);
  memcpy(head + 12, &c->channels, 4);
  if (write(user, head, sizeof head) != JC_OK) {
    free(e);
    snprintf(err, n, "write failed");
    return NULL;
  }
  return e;
}

static int32_t write_f32(jc_encoder* e, const float* data, size_t frames, char* err, size_t n) {
  if (e->write(e->user, (const uint8_t*)data, frames * e->channels * sizeof(float)) != JC_OK) {
    snprintf(err, n, "write failed");
    return JC_ERR_IO;
  }
  return JC_OK;
}

static int32_t finish(jc_encoder* e, char* err, size_t n) {
  (void)e;
  (void)err;
  (void)n;
  return JC_OK;
}

static void destroy(jc_encoder* e) { free(e); }

static const jc_encoder_plugin kPlugin = {
    JC_PLUGIN_ABI_VERSION, sizeof(jc_encoder_plugin), JC_KIND_AUDIO, "jctest", "jctest",
    "jowe-convert test encoder", "1.0", "Apache-2.0", probe, negotiate, create, write_f32, finish, destroy};

static const jc_encoder_plugin* const kList[] = {&kPlugin};

JC_PLUGIN_EXPORT const jc_encoder_plugin* const* jc_encoder_plugins(uint32_t host_abi_version, size_t* count) {
  if (host_abi_version != JC_PLUGIN_ABI_VERSION) {
    *count = 0;
    return NULL;
  }
  *count = 1;
  return kList;
}
