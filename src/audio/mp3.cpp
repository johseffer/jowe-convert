#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#if defined(__EMSCRIPTEN__)
#define MINIMP3_NO_SIMD
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "minimp3.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "audio.hpp"

namespace jowe::convert::detail {

namespace {

size_t skipId3(const Bytes& in) {
  if (in.size() >= 10 && std::memcmp(in.data(), "ID3", 3) == 0)
    return std::min(in.size(), 10 + (size_t(in[6] & 0x7f) << 21 | size_t(in[7] & 0x7f) << 14 |
                                     size_t(in[8] & 0x7f) << 7 | (in[9] & 0x7f)));
  return 0;
}

}  // namespace

bool mp3Info(const Bytes& in, AudioHeader& h) {
  mp3dec_t dec;
  mp3dec_init(&dec);
  mp3dec_frame_info_t fi;
  size_t pos = skipId3(in);
  uint64_t frames = 0, bitrateSum = 0, count = 0;
  while (pos < in.size()) {
    int samples = mp3dec_decode_frame(&dec, in.data() + pos, int(std::min<size_t>(in.size() - pos, 1 << 20)), nullptr, &fi);
    if (fi.frame_bytes == 0) break;
    pos += size_t(fi.frame_bytes);
    if (samples <= 0) continue;
    if (!count) {
      h.sampleRate = fi.hz;
      h.channels = fi.channels;
    }
    frames += uint64_t(samples);
    bitrateSum += uint64_t(fi.bitrate_kbps);
    ++count;
  }
  if (!count) return false;
  h.codec = "mp3";
  h.frames = frames;
  h.bits = 16;
  h.bitrateKbps = int(bitrateSum / count);
  return true;
}

bool mp3Read(const Bytes& in, Pcm& out, std::string& err) {
  mp3dec_t dec;
  mp3dec_init(&dec);
  mp3dec_frame_info_t fi;
  std::vector<float> frame(MINIMP3_MAX_SAMPLES_PER_FRAME);
  size_t pos = skipId3(in);
  out = Pcm{};
  out.codec = "mp3";
  out.sourceBits = 16;
  while (pos < in.size()) {
    int samples = mp3dec_decode_frame(&dec, in.data() + pos, int(std::min<size_t>(in.size() - pos, 1 << 20)),
                                      frame.data(), &fi);
    if (fi.frame_bytes == 0) break;
    pos += size_t(fi.frame_bytes);
    if (samples <= 0) continue;
    if (!out.channels) {
      out.channels = fi.channels;
      out.sampleRate = fi.hz;
    }
    if (fi.channels != out.channels) continue;
    out.samples.insert(out.samples.end(), frame.begin(), frame.begin() + size_t(samples) * fi.channels);
  }
  if (out.samples.empty()) {
    err = "no MP3 frames found";
    return false;
  }
  return true;
}

}  // namespace jowe::convert::detail
