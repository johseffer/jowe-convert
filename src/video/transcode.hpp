#pragma once

#include "../core/common.hpp"
#include "isobmff.hpp"

namespace jowe::convert::detail {

struct TranscodeCapability {
  bool available = false;
  std::string backend;
  std::string reason;
  std::vector<std::string> encoders;  // "h264", "hevc"
};
TranscodeCapability videoTranscodeCapability();

double nominalFrameRate(const MovieTrack& t);
bool needsTranscode(const VideoOptions& opt, const MovieTrack& video);

Result transcodeVideo(const Context& ctx, const fs::path& in, const Movie& source, const fs::path& out,
                      const std::string& outFormat);

// Writes a synthetic clip (moving pattern with noise, optional AAC tone) with the platform encoder.
// Used by the tests to make fixtures; codec is "avc1" or "hvc1".
Result writeSyntheticClip(const fs::path& out, int width, int height, int fps, int frames, bool audio,
                          const std::string& codec = "avc1", int bitrateKbps = 8000);

}  // namespace jowe::convert::detail
