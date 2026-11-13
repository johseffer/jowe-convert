#pragma once

#include "../core/common.hpp"

namespace jowe::convert::detail {

// Interleaved float samples in [-1, 1].
struct Pcm {
  int sampleRate = 0;
  int channels = 0;
  int sourceBits = 16;
  bool sourceFloat = false;
  std::string codec;
  std::vector<float> samples;

  size_t frames() const { return channels ? samples.size() / size_t(channels) : 0; }
};

struct AudioHeader {
  std::string codec;
  int sampleRate = 0;
  int channels = 0;
  int bits = 0;
  uint64_t frames = 0;
  int bitrateKbps = 0;
};

bool wavRead(const Bytes& in, Pcm& out, std::string& err);
bool wavInfo(const Bytes& in, AudioHeader& h);
bool wavWrite(const Pcm& pcm, int bits, Bytes& out);

bool aiffRead(const Bytes& in, Pcm& out, std::string& err);
bool aiffInfo(const Bytes& in, AudioHeader& h);
bool aiffWrite(const Pcm& pcm, int bits, Bytes& out);

bool cafRead(const Bytes& in, Pcm& out, std::string& err);
bool cafInfo(const Bytes& in, AudioHeader& h);
bool cafWrite(const Pcm& pcm, int bits, Bytes& out);

bool flacRead(const Bytes& in, Pcm& out, std::string& err);
bool flacInfo(const Bytes& in, AudioHeader& h);
bool flacWrite(const Pcm& pcm, int bits, Bytes& out, std::string& err);

bool mp3Read(const Bytes& in, Pcm& out, std::string& err);
bool mp3Info(const Bytes& in, AudioHeader& h);

// OS audio (Media Foundation on Windows, AudioToolbox on Apple).
bool osAudioCanDecode(const std::string& format, std::string* reason);
bool osAudioDecode(const fs::path& path, const std::string& format, Pcm& out, std::string& err);
bool osAudioCanEncode(const std::string& format, std::string* reason);
// May adjust sample rate / channels / bitrate to what the encoder accepts; the caller converts pcm first.
void osAudioNegotiate(const std::string& format, int& sampleRate, int& channels, int& bitrateKbps);
bool osAudioEncode(const Pcm& pcm, const std::string& format, int bitrateKbps, const fs::path& out, std::string& err);

void resample(Pcm& pcm, int targetRate);
void remix(Pcm& pcm, int targetChannels);

std::string sniffAudioFormat(const Bytes& b);
Result probeAudio(const Context* ctx, const fs::path& path, const Bytes& data, const std::string& format,
                  ProbeInfo& info);
Result convertAudio(const Context& ctx, const fs::path& in, const std::string& inFormat, const fs::path& out,
                    const std::string& outFormat, ProbeInfo& outInfo);

}  // namespace jowe::convert::detail
