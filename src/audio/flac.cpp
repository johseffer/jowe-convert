#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>

#include <cmath>

#include "audio.hpp"

namespace jowe::convert::detail {

namespace {

struct DecodeState {
  const Bytes* in;
  size_t pos = 0;
  Pcm* out;
  int bits = 0;
  bool error = false;
};

FLAC__StreamDecoderReadStatus readCb(const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t* bytes, void* ud) {
  auto* s = static_cast<DecodeState*>(ud);
  size_t n = std::min(*bytes, s->in->size() - s->pos);
  if (n == 0) {
    *bytes = 0;
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }
  std::memcpy(buffer, s->in->data() + s->pos, n);
  s->pos += n;
  *bytes = n;
  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderWriteStatus writeCb(const FLAC__StreamDecoder*, const FLAC__Frame* frame,
                                       const FLAC__int32* const buffer[], void* ud) {
  auto* s = static_cast<DecodeState*>(ud);
  const unsigned ch = frame->header.channels, n = frame->header.blocksize;
  const int bits = int(frame->header.bits_per_sample ? frame->header.bits_per_sample : unsigned(s->bits));
  if (!s->out->channels) {
    s->out->channels = int(ch);
    s->out->sampleRate = int(frame->header.sample_rate);
    s->out->sourceBits = bits;
  }
  if (int(ch) != s->out->channels) {
    s->error = true;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  const double scale = 1.0 / double(1u << (bits - 1));
  auto& v = s->out->samples;
  size_t base = v.size();
  v.resize(base + size_t(n) * ch);
  for (unsigned i = 0; i < n; ++i)
    for (unsigned c = 0; c < ch; ++c) v[base + size_t(i) * ch + c] = float(buffer[c][i] * scale);
  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void metaCb(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* m, void* ud) {
  auto* s = static_cast<DecodeState*>(ud);
  if (m->type == FLAC__METADATA_TYPE_STREAMINFO) {
    s->bits = int(m->data.stream_info.bits_per_sample);
    s->out->sampleRate = int(m->data.stream_info.sample_rate);
    s->out->channels = int(m->data.stream_info.channels);
    s->out->sourceBits = s->bits;
    if (m->data.stream_info.total_samples)
      s->out->samples.reserve(size_t(m->data.stream_info.total_samples) * m->data.stream_info.channels);
  }
}

void errorCb(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void* ud) {
  static_cast<DecodeState*>(ud)->error = true;
}

struct EncodeState {
  Bytes* out;
  size_t pos = 0;
};

FLAC__StreamEncoderWriteStatus encWrite(const FLAC__StreamEncoder*, const FLAC__byte buffer[], size_t bytes,
                                        uint32_t, uint32_t, void* ud) {
  auto* s = static_cast<EncodeState*>(ud);
  if (s->pos + bytes > s->out->size()) s->out->resize(s->pos + bytes);
  std::memcpy(s->out->data() + s->pos, buffer, bytes);
  s->pos += bytes;
  return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

FLAC__StreamEncoderSeekStatus encSeek(const FLAC__StreamEncoder*, FLAC__uint64 offset, void* ud) {
  auto* s = static_cast<EncodeState*>(ud);
  s->pos = size_t(offset);
  return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
}

FLAC__StreamEncoderTellStatus encTell(const FLAC__StreamEncoder*, FLAC__uint64* offset, void* ud) {
  *offset = static_cast<EncodeState*>(ud)->pos;
  return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}

}  // namespace

bool flacInfo(const Bytes& in, AudioHeader& h) {
  size_t at = 0;
  if (in.size() >= 10 && std::memcmp(in.data(), "ID3", 3) == 0)
    at = 10 + (size_t(in[6] & 0x7f) << 21 | size_t(in[7] & 0x7f) << 14 | size_t(in[8] & 0x7f) << 7 | (in[9] & 0x7f));
  if (in.size() < at + 4 + 4 + 34 || std::memcmp(in.data() + at, "fLaC", 4)) return false;
  const uint8_t* si = in.data() + at + 8;
  h.sampleRate = int(uint32_t(si[10]) << 12 | uint32_t(si[11]) << 4 | si[12] >> 4);
  h.channels = ((si[12] >> 1) & 7) + 1;
  h.bits = (((si[12] & 1) << 4) | (si[13] >> 4)) + 1;
  h.frames = uint64_t(si[13] & 0xF) << 32 | uint64_t(si[14]) << 24 | uint64_t(si[15]) << 16 |
             uint64_t(si[16]) << 8 | si[17];
  h.codec = "flac";
  return h.sampleRate > 0;
}

bool flacRead(const Bytes& in, Pcm& out, std::string& err) {
  FLAC__StreamDecoder* dec = FLAC__stream_decoder_new();
  if (!dec) { err = "FLAC decoder allocation failed"; return false; }
  DecodeState s{&in, 0, &out};
  out = Pcm{};
  out.codec = "flac";
  bool ok = FLAC__stream_decoder_init_stream(dec, readCb, nullptr, nullptr, nullptr, nullptr, writeCb, metaCb,
                                             errorCb, &s) == FLAC__STREAM_DECODER_INIT_STATUS_OK &&
            FLAC__stream_decoder_process_until_end_of_stream(dec);
  FLAC__stream_decoder_finish(dec);
  FLAC__stream_decoder_delete(dec);
  if (!ok || out.samples.empty() || !out.channels) {
    err = "corrupt or unsupported FLAC";
    return false;
  }
  out.codec = "flac";
  return true;
}

bool flacWrite(const Pcm& pcm, int bits, Bytes& out, std::string& err) {
  FLAC__StreamEncoder* enc = FLAC__stream_encoder_new();
  if (!enc) { err = "FLAC encoder allocation failed"; return false; }
  FLAC__stream_encoder_set_channels(enc, unsigned(pcm.channels));
  FLAC__stream_encoder_set_bits_per_sample(enc, unsigned(bits));
  FLAC__stream_encoder_set_sample_rate(enc, unsigned(pcm.sampleRate));
  FLAC__stream_encoder_set_compression_level(enc, 5);
  FLAC__stream_encoder_set_total_samples_estimate(enc, pcm.frames());
  out.clear();
  EncodeState s{&out};
  if (FLAC__stream_encoder_init_stream(enc, encWrite, encSeek, encTell, nullptr, &s) !=
      FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
    FLAC__stream_encoder_delete(enc);
    err = "FLAC encoder rejected the format (" + std::to_string(pcm.channels) + " ch, " +
          std::to_string(pcm.sampleRate) + " Hz)";
    return false;
  }
  const double scale = double(1u << (bits - 1));
  const int32_t maxv = int32_t(scale - 1), minv = -int32_t(scale);
  std::vector<FLAC__int32> block;
  const size_t chunk = 4096, frames = pcm.frames();
  bool ok = true;
  for (size_t f = 0; f < frames && ok; f += chunk) {
    size_t n = std::min(chunk, frames - f);
    block.resize(n * pcm.channels);
    for (size_t i = 0; i < n * pcm.channels; ++i) {
      double v = std::floor(double(pcm.samples[f * pcm.channels + i]) * scale + 0.5);
      block[i] = FLAC__int32(std::clamp<double>(v, minv, maxv));
    }
    ok = FLAC__stream_encoder_process_interleaved(enc, block.data(), unsigned(n));
  }
  ok = FLAC__stream_encoder_finish(enc) && ok;
  FLAC__stream_encoder_delete(enc);
  if (!ok) err = "FLAC encoding failed";
  return ok;
}

}  // namespace jowe::convert::detail
